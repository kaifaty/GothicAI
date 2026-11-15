#!/usr/bin/env python3
"""Build a staging set with the cart track's own material: two layers by the world, the ribbon's maps in the uv.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/landscape_meadow_road.py meadow-road-1 [--from meadow-ground-1] [--again] \
        [--wander 14 --fade .7 --ragged 1.3] [--set NAME=VALUE ...] [--preview DIR] [--joints BASE]

reports/engine/2026-11-15-meadow-road. Copies a set with the meadow's ground, gives the five road textures the
`track` field beside their `verge`, and the meadow's `ground` the `border`: the road's loose layer.

- crust, loose: the two layers that lie by the world's x and z, from two scans (Poly Haven, CC0) by numpy alone.
  What a scan has longer than FLAT_M is levelled in colour and in relief - a mark of it would come back with every
  repeat along the road (the loose scan carries a tyre's tread: its relief is cut shorter). The crust's mean
  colour is the painted road's crown, lifted so far that the footpath's straw is a tint of it; the loose layer's is
  the mean of the meadow's earth: one earth on the road's strip and in the meadow's notches. In the blue of the
  ORMs the height, ranked per mip (landscape_fringe.write_orm): the shader lets the higher layer win where the wear
  is between;
- the ribbon's maps of each road texture, 512 px in its own uv, drawn here from curves and a profile: `tint` (the
  ruts' beds darker and redder, the footpath's straw), `normal` (the ruts' walls, the berm beside, the trodden
  middle; slopes in the ribbon's own metres per u and per v) and `mask` - red the occlusion of the large forms,
  green the wear (0 the crust, 1 the loose earth), blue the ribbon's own share (0 at its painted edge). Along v
  the ruts of the road stand still - the world's pieces of the T-cross meet it at any v, and u turned over; they
  wander, fade and fray by the world's noise in the shader, which no joint cuts. Only the stains and the depth
  change along v.

No pixel of the original goes into a map: its textures are read for mean colours only (and for the previews).
`--again` rebuilds the maps of a set this script made (it holds `meadow-road.json`); no other set is written to.
`--set` gives a constant of this file another value for a probe (`--set HALF_BED=.035 RUT=17,.6,.04`: a colour by
its hue, saturation and luminance); the set's `meadow-road.json` keeps what was set.
`--preview` writes the tiles and a strip of the road as the shader composes it (the rule of grass_track.glsl in
numpy: seen from above, lit low, and from the game's camera along the track beside the painted road), prints the
bands' means against the painted ones and the strip's autocorrelation, and stops. `--joints` reads the base's
chunks and prints how far the maps of two road textures differ along the edges they share.
"""
from __future__ import annotations

import argparse
import colorsys
import importlib.util
import json
import shutil
from pathlib import Path

import numpy as np
from scipy.ndimage import gaussian_filter, map_coordinates

HERE = Path(__file__).parent


def sibling(name: str):
    path = HERE / name
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ground = sibling('landscape_meadow_ground.py')
SOURCES, STAGING, MEADOW, ROADS, luma = ground.SOURCES, ground.STAGING, ground.MEADOW, ground.ROADS, ground.luma
CRUST, LOOSE = 'dry_ground_rocks', 'dry_mud_field_001'     # Poly Haven, CC0
CRUST_TILE_M, LOOSE_TILE_M = 4., 3.
LAYER_SIZE, RIBBON_SIZE = 2048, 512
FLAT_M = {'crust': .7, 'loose': .16}    # what the scan has longer than this is levelled
LEVEL = .85
RELIEF = {'crust': .8, 'loose': .6}     # the scan's slopes times this
CONTRAST = {'crust': 1.1, 'loose': 1.}
ROUGH_FLOOR = .85
KNOBS = dict(wander=14., fade=.7, ragged=1.3)   # the record's: the drawing's sway in cm, the ruts' fading, the fray
OCCLUSION = 1.              # of the layers' and the ribbon's occlusion: 0 for a probe of what it does in the light


def from_hsy(hue: float, saturation: float, level: float) -> np.ndarray:
    rgb = np.array(colorsys.hsv_to_rgb(hue / 360, saturation, 1.))
    return rgb * (level / luma(rgb))


# The painted road's bands, linear light (the note, "Нарисованная PATH_06 по полосам"): what the layers and the
# tint are toned to, as (hue, saturation, luminance). The crust has the crown's hue - where the tint is none, at
# the ribbon's edge, it must not turn to straw - and is lighter than it by so much that the footpath's straw is a
# tint of it under 1 (crust_tone)
CROWN = (32., .62, .100)        # PATH_06: its middle and shoulders
STRAW = (42.3, .71, .1075)      # PATH_05: its trodden earth
RUT = (14., .62, .026)          # PATH_06: the bed of a rut, the loose layer under its tint
PAINTED_GRASS = from_hsy(50.4, .83, .0764)
# The mean of the crust against crust_tone(), linear RGB: 1 - at the eye a lit layer has the tone of the paint
# beside it. From afar the paint is darker (its mips are means of the encoded colour: 0.86-0.89 of the linear mean
# at the third and fourth), and so is the track - GrassTrackFarShade of grass_track.glsl
CRUST_SCREEN = (1., 1., 1.)

# The profile of PATH_06 across, in shares of u (3.86 m to a u on the pilot's median ribbon), symmetric about 0.5:
# the world lays the T-cross on the road with u turned over. The painted ruts stand at 0.36 and 0.68
RUTS = (.34, .66)
HALF_BED = .03              # half the bed of a rut
WALL = .035                 # its wall
CRUMBS = .045               # the loose earth thrown out of it fades over this
DEPTH_M, BERM_M, PATH_M = .06, .022, .015
BED_WEAR, CRUMBS_WEAR, PATH_WEAR = .76, .34, .15    # what each adds to the wear of the crown, 0.16
# The share of the rut's tone in the tint: the painted rut is a dark line of the wheel's width in a wider stain.
# The line is the tint's alone - as a form it was a black crack at noon (the first probe)
CORE = (.012, .034)         # from the rut's middle: the dark line whole, then gone
CORE_DARK, BED_DARK, CRUMBS_DARK = .55, .6, .4
CRUST_TINT = .6             # grass_track.glsl's GrassTrackCrustTint: what of the tint the crust takes
EDGE = (.02, .06)           # the ribbon's own share grows from 0 to 1 over this
EDGE_05 = (.14, .22)        # of the footpath: its painted grass is wider
SHOULDER = (.06, .22)       # loose at the ribbon's edge, the crust from here on
METRIC = {                  # metres to a u and to a v, the pilot's medians (road_census.py)
    'NW_NATURE_PATH_06': (3.86, 5.38), 'NW_NATURE_PATH_06_TCROSS': (4.84, 4.25), 'NW_NATURE_PATH_06_END': (3.86, 5.38),
    'NW_NATURE_PATH_05': (3.31, 4.32), 'NW_NATURE_PATH_05_TO_06': (3.86, 5.38)}
FOLDER = {'NW_NATURE_PATH_06': 'road_path_06', 'NW_NATURE_PATH_06_TCROSS': 'road_path_06_tcross',
          'NW_NATURE_PATH_06_END': 'road_path_06_end', 'NW_NATURE_PATH_05': 'road_path_05',
          'NW_NATURE_PATH_05_TO_06': 'road_path_05_to_06'}
END_VERGE = dict(reach=120, margin=100)     # the end piece is all border


def crust_tone() -> np.ndarray:
    crown, straw = from_hsy(*CROWN), from_hsy(*STRAW)
    return crown * (straw / crown).max()


def override(pairs: list[str]) -> dict:
    """--set: constants of this file by name; a colour by three numbers."""
    done = {}
    for pair in pairs:
        name, _, text = pair.partition('=')
        if name not in globals() or not name.isupper():
            raise SystemExit(f'--set {name}: no such constant')
        value = [float(part) for part in text.split(',')]
        old = globals()[name]
        if isinstance(old, dict):
            raise SystemExit(f'--set {name}: not a number or a row of them')
        if np.size(old) != len(value):
            raise SystemExit(f'--set {name}: {np.size(old)} numbers, not {len(value)}')
        globals()[name] = value[0] if np.isscalar(old) else tuple(value)
        done[name] = value[0] if np.isscalar(old) else value
    return done


def smooth(value, low=0., high=1.):
    t = np.clip((value - low) / (high - low), 0, 1)
    return t * t * (3 - 2 * t)


# ---- the layers ----

def scan_layer(pilot, kind: str, scan: str, tile: float, size: int, target: np.ndarray, screen) -> dict:
    folder = SOURCES / scan
    colour = pilot.linear(pilot.load(folder / f'{scan}_diff_4k.jpg', size))
    normal = pilot.load(folder / f'{scan}_nor_gl_4k.png', size).astype(np.float32) / 127.5 - 1
    arm = pilot.load(folder / f'{scan}_arm_4k.jpg', size).astype(np.float32) / 255
    height = pilot.load(folder / f'{scan}_disp_4k.png', size, 'L').astype(np.float32) / 255
    sigma = FLAT_M[kind] / 4 / (tile / size)

    def high(field):
        return field - gaussian_filter(field, sigma, mode='wrap')
    low = np.stack([gaussian_filter(colour[..., c], FLAT_M['crust'] / 4 / (tile / size), mode='wrap') for c in range(3)], axis=2)
    value = colour / (low / low.reshape(-1, 3).mean(axis=0)) ** LEVEL
    value, gains = ground.to_mean(ground.spread(value, CONTRAST[kind]), target, screen)
    z = np.maximum(normal[..., 2], .05)
    # the scan's normal is +Y up the image (OpenGL): slopes of the height along x and down the image
    slope = np.stack([high(-normal[..., 0] / z), high(normal[..., 1] / z)], axis=2) * RELIEF[kind]
    rough = ROUGH_FLOOR + (1 - ROUGH_FLOOR) * arm[..., 1]
    return dict(albedo=value, slope=slope, orm=np.stack([1 - OCCLUSION * (1 - arm[..., 0]), rough, high(height)], axis=2),
                gains=gains)


def write_layer(directory: Path, maps: dict, dds, pilot, layer, fringe) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    layer.write_albedo(dds, pilot, directory / 'albedo.dds', maps['albedo'])
    slope = maps['slope'].astype(np.float32)
    # the engine's bitangent follows texture v down the image: the slopes as they are
    dds.write_dds(directory / 'normal.dds', np.dstack([-slope, np.ones(slope.shape[:2], np.float32)]), normal=True)
    orm = maps['orm'].astype(np.float64).copy()
    orm[..., :2] *= 255
    fringe.write_orm(dds, directory / 'orm.dds', orm)


# ---- the ribbon's maps ----

def line(size: int, waves: tuple[float, float], seed: int) -> np.ndarray:
    """A periodic noise along v, a column: wavelengths between the two, shares of v; mean 0, spread 1, within 2."""
    freq = np.abs(np.fft.fftfreq(size, d=1 / size))
    band = (freq >= 1 / waves[1]) & (freq <= 1 / waves[0])
    value = np.fft.ifft(np.fft.fft(np.random.default_rng(seed).standard_normal(size)) * band / np.maximum(freq, 1)).real
    return np.clip((value - value.mean()) / max(value.std(), 1e-9), -2, 2)[:, None].astype(np.float32)


def grid(size: int) -> tuple[np.ndarray, np.ndarray]:
    at = (np.arange(size, dtype=np.float32) + .5) / size
    return at[None, :], at[:, None]


def straight_ruts(size: int, seed: int, still: np.ndarray) -> np.ndarray:
    """The distance past the bed's edge of the nearer of two ruts along v, in u; under 0 on the bed. still: 0 where
    the ruts stand at their own place and width - on the edges other pieces meet."""
    u, _ = grid(size)
    result = np.full((size, size), np.inf, np.float32)
    for i, centre in enumerate(RUTS):
        at = centre + .022 * line(size, (.3, 1.), seed + i) * still
        half = HALF_BED * (1 + .4 * line(size, (.12, .6), seed + 10 + i) * still)
        result = np.minimum(result, np.abs(u - at) - half)
    return result


def path06(size: int, seed: int) -> dict:
    u, v = grid(size)
    inside = np.minimum(u, 1 - u) + 0 * v
    middle = 1 - smooth(np.abs(u - .5), .04, .075) + 0 * v
    return dict(sd=straight_ruts(size, seed, 0 * v), gain=1., inside=inside, edge=EDGE, straw=0., path=middle * .7,
                wrap=True)


def tcross(size: int, seed: int) -> dict:
    """The road along v, a branch leaving through u = 1: there its profile lies along v. The wheels turn from the
    branch into the road both ways by quarter circles about the two corners."""
    u, v = grid(size)
    sd = np.full((size, size), np.inf, np.float32)
    for centre in RUTS:
        sd = np.minimum(sd, np.abs(u - centre) - HALF_BED + 0 * v)
        for corner in (0., 1.):
            sd = np.minimum(sd, np.abs(np.hypot(1 - u, v - corner) - centre) - HALF_BED)
    inside = np.minimum(u, np.maximum(1 - u, np.minimum(v, 1 - v)))
    branch = (1 - smooth(np.abs(v - .5), .04, .075)) * smooth(u, .5, .6)
    return dict(sd=sd, gain=1., inside=inside, edge=EDGE, straw=0.,
                path=np.maximum(1 - smooth(np.abs(u - .5), .04, .075) + 0 * v, branch) * .7, wrap=False)


def end(size: int, seed: int) -> dict:
    """The road at v = 0; toward v = 1 the ruts grow shallow and the earth gives way to the paint, the beds last."""
    u, v = grid(size)
    sd = straight_ruts(size, seed, smooth(v, .05, .3) * (1 - smooth(v, .9, 1.)))
    far = v + .07 * ground.noise(size, 1., (.08, .4), seed + 30, 1.)
    inside = np.minimum(np.minimum(u, 1 - u), (.86 - far + .1 * (1 - smooth(sd, 0., .06))) * .5)
    return dict(sd=sd, gain=1 - smooth(far, .15, .7), inside=inside, edge=EDGE, straw=0.,
                path=(1 - smooth(np.abs(u - .5), .04, .075)) * (1 - smooth(far, .3, .7)) * .7, wrap=False)


def path05(size: int, seed: int) -> dict:
    """The footpath: no ruts, a trodden band in the middle, its edge wandering."""
    u, v = grid(size)
    still = np.sin(2 * np.pi * v) ** 2
    at = .5 + .03 * line(size, (.3, 1.), seed) * still
    half = .2 * (1 + .2 * line(size, (.15, .6), seed + 1) * still)
    return dict(sd=np.full((size, size), np.inf, np.float32), gain=0., inside=np.minimum(u, 1 - u) + 0 * v, edge=EDGE_05,
                straw=1., path=1 - smooth(np.abs(u - at) - half, -.05, .05), wrap=True)


def path05_to_06(size: int, seed: int) -> dict:
    """The footpath at v = 0 turns into the track at v = 1."""
    u, v = grid(size)
    track = smooth(v + .06 * ground.noise(size, 1., (.08, .4), seed + 30, 1.), .25, .75)
    walk = 1 - smooth(np.abs(u - .5) - .2, -.05, .05)
    middle = 1 - smooth(np.abs(u - .5), .04, .075)
    edge = (EDGE_05[0] + (EDGE[0] - EDGE_05[0]) * smooth(v, .2, .8), EDGE_05[1] + (EDGE[1] - EDGE_05[1]) * smooth(v, .2, .8))
    return dict(sd=straight_ruts(size, seed, smooth(v, .1, .4) * (1 - smooth(v, .9, 1.))), gain=track,
                inside=np.minimum(u, 1 - u) + 0 * v, edge=edge, straw=1 - track,
                path=walk * (1 - track) + middle * track * .7, wrap=False)


DRAW = {'NW_NATURE_PATH_06': path06, 'NW_NATURE_PATH_06_TCROSS': tcross, 'NW_NATURE_PATH_06_END': end,
        'NW_NATURE_PATH_05': path05, 'NW_NATURE_PATH_05_TO_06': path05_to_06}


def ribbon_maps(fields: dict, metric: tuple[float, float], seed: int) -> dict:
    size = fields['inside'].shape[0]

    def n(waves, salt, slope=1.):
        return ground.noise(size, 1., waves, seed + 100 + salt, slope)
    gain = fields['gain']
    sd = fields['sd'] + .006 * n((.03, .2), 1)                      # no ruled walls
    bed = (1 - smooth(sd, 0., WALL)) * gain
    crumbs = np.exp(-np.maximum(sd, 0) / CRUMBS) * gain * (1 - bed)
    own = smooth(fields['inside'] + .012 * n((.04, .25), 2), fields['edge'][0], fields['edge'][1])
    loose_edge = 1 - smooth(fields['inside'], fields['edge'][1], fields['edge'][1] + SHOULDER[1] - SHOULDER[0])
    path = fields['path'] * np.clip(.75 + .5 * n((.1, .6), 3, 1.5), 0, 1)

    # the height, metres: the beds with their lumps, the berm beside, the trodden middle, slow bumps
    deep = DEPTH_M * np.clip(.85 + .22 * n((.25, 1.), 4, 1.5), .4, 1.3)
    berm = BERM_M * np.exp(-((sd - WALL - .012) / .016) ** 2) * gain * np.clip(.6 + .6 * n((.1, .5), 5), 0, 1.4)
    height = -deep * bed * (1 + .18 * n((.04, .15), 6)) + berm - PATH_M * path + .005 * n((.1, .4), 7, 1.)
    height = height * smooth(own)
    if fields['wrap']:
        along = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * size / 2
    else:
        along = np.gradient(height, axis=0) * size
    across = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * size / 2
    slope = np.stack([across / metric[0], along / metric[1]], axis=2)

    # the wear: 0 the crust, 1 the loose earth; at one half the world's noise and the layers' heights choose
    wear = (.16 + BED_WEAR * bed + CRUMBS_WEAR * crumbs + .5 * loose_edge * (1 - bed) + PATH_WEAR * path * (1 - bed) +
            .07 * n((.05, .3), 8))
    # where a rut is not there the earth of an end piece is loose: it thins out into the paint
    wear = np.clip(wear, 0, 1)
    occlusion = 1 - OCCLUSION * (1 - np.clip(1 - .30 * bed * deep / DEPTH_M - .35 * bed * (1 - bed), .5, 1))

    # the tint: the beds dark and red, stains of them beside; the crown's and the footpath's own tone of the crust;
    # at the ribbon's edge none - there the loose earth is the meadow's
    stain = np.clip(.45 + .5 * n((.08, .6), 9, 1.5), 0, 1)
    core = (1 - smooth(fields['sd'] + HALF_BED + .004 * n((.03, .2), 11), CORE[0], CORE[1])) * gain
    dark = np.clip(core * (CORE_DARK + .2 * n((.05, .4), 12, 1.)) + bed * (BED_DARK + .25 * n((.06, .5), 10, 1.)) +
                   CRUMBS_DARK * crumbs * stain, 0, 1)[..., None]
    straw = (np.zeros((size, size), np.float32) + fields['straw'])[..., None]
    base = (from_hsy(*CROWN) * (1 - straw) + from_hsy(*STRAW) * straw) / crust_tone()
    tint = base * (1 - dark) + np.clip(from_hsy(*RUT) / fields['loose_tone'], 0, 1) * dark
    inner = smooth(fields['inside'], fields['edge'][1], fields['edge'][1] + .1)[..., None]
    tint = np.clip(1 - (1 - tint) * inner, 0, 1)
    return dict(tint=tint.astype(np.float32), slope=slope.astype(np.float32), height=height,
                mask=np.stack([occlusion, wear, own], axis=2).astype(np.float32))


def write_ribbon(directory: Path, maps: dict, dds, pilot, layer) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    layer.write_albedo(dds, pilot, directory / 'tint.dds', maps['tint'].astype(np.float64))
    slope = maps['slope']
    # x where u grows, y where v grows: the shader lays each on the ribbon's own axis
    dds.write_dds(directory / 'normal.dds', np.dstack([-slope, np.ones(slope.shape[:2], np.float32)]), normal=True)
    dds.write_dds(directory / 'mask.dds', maps['mask'])


# ---- the shader's rule in numpy: what a strip of the road comes to ----

def value_noise(px: np.ndarray, pz: np.ndarray, step: int, salt: int) -> np.ndarray:
    """grassValueNoise of grass_common.glsl, as grassTrackNoise: -1..1."""
    def cell(ix, iz):
        h = (ix.astype(np.uint32) * np.uint32(0x9E3779B1)) ^ (iz.astype(np.uint32) * np.uint32(0x85EBCA77)) ^ np.uint32(salt)
        h ^= h >> np.uint32(15)
        h *= np.uint32(0x2C1B3C6D)
        h ^= h >> np.uint32(12)
        h *= np.uint32(0x297A2D39)
        h ^= h >> np.uint32(15)
        return ((h >> np.uint32(8)) & np.uint32(0xFF)).astype(np.int64)
    ix, iz = px // step, pz // step
    tx, tz = ((px - ix * step) * 256) // step, ((pz - iz * step) * 256) // step
    sx, sz = (tx * tx * (768 - 2 * tx)) >> 16, (tz * tz * (768 - 2 * tz)) >> 16
    top = cell(ix, iz) * (256 - sx) + cell(ix + 1, iz) * sx
    bottom = cell(ix, iz + 1) * (256 - sx) + cell(ix + 1, iz + 1) * sx
    return ((((top * (256 - sz) + bottom * sz) >> 8) - 32640) / 32640).astype(np.float32)


def rank(height: np.ndarray) -> np.ndarray:
    order = np.argsort(height, axis=None, kind='stable')
    result = np.empty(height.size, np.float32)
    result[order] = np.arange(height.size) / (height.size - 1)
    return result.reshape(height.shape)


def composed(crust: dict, loose: dict, ribbon: dict, metric, knobs: dict, length_m=40., texel_m=.02,
             heading=.37, origin=(3790000., 110000.), sun=(.25, -.1, .2)) -> dict:
    """A straight piece of the road as grass_track.glsl composes it near the eye: rows across, columns along.
    Returns the colour, the colour lit by a low sun, and which layer each pixel is."""
    width, period = metric
    across = (np.arange(int(width / texel_m)) + .5) * texel_m
    along = (np.arange(int(length_m / texel_m)) + .5) * texel_m
    x, y = across[:, None], along[None, :]
    u, v = x / width + 0 * y, y / period + 0 * x
    c, s = np.cos(heading), np.sin(heading)
    wx, wz = origin[0] + (c * x - s * y) * 100, origin[1] + (s * x + c * y) * 100      # cm
    px, pz = np.floor(wx * 16).astype(np.int64), np.floor(wz * 16).astype(np.int64)

    def noise(step, salt, shift=(0, 0)):
        return value_noise(px + shift[0], pz + shift[1], step, salt)
    quick = noise(2560, 0x73) * .3
    sway_x, sway_z = (noise(11200, 0x71) + quick) * knobs['wander'], (noise(11200, 0x72, (5113, 2971)) - quick) * knobs['wander']
    fu, fv = u % 1, v % 1
    hold = smooth(np.minimum(fu, 1 - fu), 0, .08), smooth(np.minimum(fv, 1 - fv), 0, .08)
    ur = u + np.clip((c * sway_x + s * sway_z) / (width * 100), -.1, .1) * hold[0]
    vr = v + np.clip((-s * sway_x + c * sway_z) / (period * 100), -.1, .1) * hold[1]

    def ribbon_at(image):
        size = image.shape[0]
        return image[np.floor(vr * size).astype(int) % size, np.floor(ur * size).astype(int) % size]

    def layer_at(image, tile, turn):
        size = image.shape[0]
        lu, lv = (turn[0] * wx + turn[1] * wz) / (tile * 100), (-turn[1] * wx + turn[0] * wz) / (tile * 100)
        return image[np.floor(lv * size).astype(int) % size, np.floor(lu * size).astype(int) % size]
    turn_c, turn_l = (.84805, -.52992), (.93358, .35837)
    mask = ribbon_at(ribbon['mask'])
    k = 1 - knobs['fade'] * smooth(noise(9600, 0x75) * .6 + noise(3520, 0x76) * .4, .05, .55)
    lean = mask[..., 1] * (1 + (k - 1) * .5) * 256 - 128 + (noise(3840, 0x77) * 90 + noise(560, 0x78) * 40) * knobs['ragged']
    high_c = layer_at(rank(crust['orm'][..., 2]), CRUST_TILE_M, turn_c)
    high_l = layer_at(rank(loose['orm'][..., 2]), LOOSE_TILE_M, turn_l)
    is_loose = np.where(np.abs(lean) < 70, lean + (high_l - high_c) * 140 > 0, lean > 0)
    height = np.where(is_loose, high_l, high_c)
    own = mask[..., 2]
    mine = (own >= 1) | (own * 256 + (height - .5) * 140 > 128)
    tone = 1 + noise(12800, 0x79) * .08 + noise(2080, 0x7A) * .05
    colour = np.where(is_loose[..., None], layer_at(loose['albedo'], LOOSE_TILE_M, turn_l), layer_at(crust['albedo'], CRUST_TILE_M, turn_c))
    colour = colour * (1 + (ribbon_at(ribbon['tint']) - 1) * (k * np.where(is_loose, 1., CRUST_TINT))[..., None]) * tone[..., None]
    colour = np.where(mine[..., None], colour, PAINTED_GRASS)

    # the light of a low sun from the side: the ribbon's slopes on its own axes, the layers' turned back to them
    def turned(slope, turn):
        gx, gz = turn[0] * slope[..., 0] - turn[1] * slope[..., 1], turn[1] * slope[..., 0] + turn[0] * slope[..., 1]
        return np.stack([c * gx + s * gz, -s * gx + c * gz], axis=2)
    slope = ribbon_at(ribbon['slope']) * k[..., None]
    slope = slope + np.where(is_loose[..., None], turned(layer_at(loose['slope'], LOOSE_TILE_M, turn_l), turn_l),
                             turned(layer_at(crust['slope'], CRUST_TILE_M, turn_c), turn_c))
    normal = np.dstack([-slope, np.ones(slope.shape[:2], np.float32)])
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    light = np.array(sun) / np.linalg.norm(sun)
    lit = colour * (np.clip(normal @ light, 0, None) / light[2] * .8 + .2)[..., None]
    return dict(colour=colour, lit=np.where(mine[..., None], lit, PAINTED_GRASS), loose=is_loose, mine=mine, k=k)


def painted_strip(pilot, texture: str, metric, length_m=40., texel_m=.02) -> np.ndarray:
    from PIL import Image
    base = sorted(Path.home().glob('.cache/gothic-ai/base/*/textures'), key=lambda p: p.stat().st_mtime)[-1]
    with Image.open(base / f'{texture}.dds') as image:
        paint = pilot.linear(np.asarray(image.convert('RGB')))
    rows, columns = int(metric[0] / texel_m), int(length_m / texel_m)
    u = (np.arange(rows) + .5) * texel_m / metric[0]
    v = (np.arange(columns) + .5) * texel_m / metric[1]
    # the texture's u across the image, its v down it
    return paint[(np.floor(v * paint.shape[0]).astype(int) % paint.shape[0])[None, :],
                 (np.floor(u * paint.shape[1]).astype(int) % paint.shape[1])[:, None]]


def from_camera(strip: np.ndarray, texel_m: float, size=(960, 540), eye=(1.9, -2.5), pitch=-.2, fov=1.05) -> np.ndarray:
    """The strip (rows across, columns along) from the game's camera: over the road's middle, looking along it."""
    sub = 3
    width, height = size[0] * sub, size[1] * sub
    focal = height / 2 / np.tan(fov / 2)
    sx, sy = np.meshgrid(np.arange(width) - width / 2 + .5, np.arange(height) - height / 2 + .5)
    # forward along the strip, right across it, down with the image's rows
    forward = np.cos(pitch) * focal + np.sin(pitch) * sy
    down = -np.sin(pitch) * focal + np.cos(pitch) * sy
    hit = down > 1e-3
    reach = np.where(hit, eye[0] / np.maximum(down, 1e-3), 0)
    along, across = eye[1] + forward * reach, sx * reach + strip.shape[0] * texel_m / 2
    row, column = across / texel_m - .5, along / texel_m - .5
    inside = hit & (row >= 0) & (row < strip.shape[0] - 1) & (column >= 0) & (column < strip.shape[1] - 1)
    image = np.stack([map_coordinates(strip[..., c], [np.clip(row, 0, strip.shape[0] - 1), np.clip(column, 0, strip.shape[1] - 1)],
                                      order=1) for c in range(3)], axis=2)
    image = np.where(inside[..., None], image, np.where(hit[..., None], PAINTED_GRASS * .8, np.array((.25, .32, .45))))
    return image.reshape(size[1], sub, size[0], sub, 3).mean(axis=(1, 3))


def band_means(strip: np.ndarray, bands) -> dict:
    rows = strip.shape[0]
    result = {}
    for name, low, high in bands:
        mean = strip[int(low * rows):int(high * rows)].reshape(-1, 3).mean(axis=0)
        hue, saturation, _ = colorsys.rgb_to_hsv(*mean)
        result[name] = dict(luminance=round(float(luma(mean)), 4), hue=round(hue * 360, 1), saturation=round(saturation, 2))
    return result


def repeat(strip: np.ndarray, texel_m: float, shifts=(3., 4., 12.), cell_m=.1) -> dict:
    """The autocorrelation of the strip's luminance along the road, the mean of each row across taken out: what
    comes back with a shift, apart from the profile itself. In cells of cell_m: what the eye holds from afar."""
    cell = int(cell_m / texel_m)
    value = luma(strip)
    value = value[:value.shape[0] // cell * cell, :value.shape[1] // cell * cell]
    value = value.reshape(value.shape[0] // cell, cell, value.shape[1] // cell, cell).mean(axis=(1, 3))
    value = value - value.mean(axis=1, keepdims=True)
    result = {}
    for shift in shifts:
        step = int(round(shift / cell_m))
        a, b = value[:, :-step], value[:, step:]
        result[f'{shift:g} m'] = round(float((a * b).mean() / np.sqrt((a * a).mean() * (b * b).mean())), 3)
    return result


def preview(folder: Path, pilot, crust: dict, loose: dict, ribbons: dict, knobs: dict) -> dict:
    from PIL import Image
    folder.mkdir(parents=True, exist_ok=True)
    report = {}
    for name, maps, tile in (('crust', crust, CRUST_TILE_M), ('loose', loose, LOOSE_TILE_M)):
        # the tile whole and a square metre of it texel for texel
        whole = Image.fromarray(pilot.encode(maps['albedo']))
        whole.resize((1024, 1024), Image.BOX).save(folder / f'{name}-tile.png')
        metre = int(maps['albedo'].shape[0] / tile)
        whole.crop((0, 0, metre, metre)).save(folder / f'{name}-near.png')
    for texture, maps in ribbons.items():
        sheet = np.concatenate([maps['tint'], maps['mask'], np.clip(.5 - np.dstack([maps['slope'], np.zeros_like(maps['height'])]) * .5, 0, 1)], axis=1)
        Image.fromarray((np.clip(sheet, 0, 1) * 255).astype(np.uint8)).save(folder / f'{FOLDER[texture]}-maps.png')
    census = sibling('../../reports/engine/2026-11-15-meadow-road/road_census.py')
    for texture in ('NW_NATURE_PATH_06', 'NW_NATURE_PATH_05'):
        made = composed(crust, loose, ribbons[texture], METRIC[texture], knobs)
        paint = painted_strip(pilot, texture, METRIC[texture])
        gap = np.zeros((6, paint.shape[1], 3))
        Image.fromarray(pilot.encode(np.concatenate([paint, gap, made['colour'], gap, made['lit']], axis=0))).save(
            folder / f'{FOLDER[texture]}-strip.png')
        Image.fromarray(pilot.encode(np.concatenate([from_camera(paint, .02), from_camera(made['colour'], .02),
                                                     from_camera(made['lit'], .02)], axis=1))).save(
            folder / f'{FOLDER[texture]}-along.png')
        bands = census.BANDS[texture] + (('all', 0., 1.),)
        report[texture] = dict(painted=band_means(paint, bands), made=band_means(made['colour'], bands),
                               loose_share=round(float(made['loose'][made['mine']].mean()), 3),
                               repeat=repeat(made['colour'], .02), repeat_painted=repeat(paint, .02))
        if texture == 'NW_NATURE_PATH_06':
            rows = made['colour'].shape[0]
            def mean_of(low, high):
                return float(luma(made['colour'][int(low * rows):int(high * rows)]).mean())
            report[texture]['rut_to_crown'] = round(sum(mean_of(at - .03, at + .03) for at in RUTS) / 2 / mean_of(.44, .56), 3)
    return report


def joints(base: Path, ribbons: dict) -> dict:
    """Along every edge two road textures share in the pilot: how far their ribbons' maps differ, the mean and the
    worst of the wear, the own share and the tint's luma over points of the edge."""
    census = sibling('../../reports/engine/2026-11-15-meadow-road/road_census.py')
    edges = {}
    for path in sorted((base / 'worlds/NEWWORLD/chunks').glob('*.glb')):
        x, z = (int(part) for part in path.stem.split('_')[-2:])
        if not census.in_pilot((x, z)):
            continue
        for name, pos, uv, tris in census.primitives(path):
            if name not in ROADS:
                continue
            for tri in tris:
                for a, b in ((0, 1), (1, 2), (2, 0)):
                    pa, pb = tuple(np.round(pos[tri[a]]).astype(int)), tuple(np.round(pos[tri[b]]).astype(int))
                    first = pa <= pb
                    edges.setdefault((min(pa, pb), max(pa, pb)), {})[name] = (uv[tri[a]], uv[tri[b]]) if first else (uv[tri[b]], uv[tri[a]])

    def read(texture, start, stop):
        maps, share = ribbons[texture], np.linspace(.02, .98, 49)[:, None]
        at = start[None, :] * (1 - share) + stop[None, :] * share
        size = maps['mask'].shape[0]
        row, column = np.floor(at[:, 1] * size).astype(int) % size, np.floor(at[:, 0] * size).astype(int) % size
        return np.column_stack([maps['mask'][row, column, 1], maps['mask'][row, column, 2], luma(maps['tint'][row, column])])
    result = {}
    for key, sides in edges.items():
        if len(sides) < 2:
            continue
        (first, a), (second, b) = sorted(sides.items())
        gap = np.abs(read(first, *a) - read(second, *b))
        metres = float(np.linalg.norm(np.array(key[0]) - np.array(key[1])) / 100)
        entry = result.setdefault(f'{first[10:]} - {second[10:]}', dict(edges=0, metres=0., mean=np.zeros(3), worst=np.zeros(3)))
        entry['edges'] += 1
        entry['metres'] += metres
        entry['mean'] += gap.mean(axis=0) * metres
        entry['worst'] = np.maximum(entry['worst'], gap.max(axis=0))
    for entry in result.values():
        entry['mean'] = dict(zip(('wear', 'own', 'tint'), (entry['mean'] / entry['metres']).round(3).tolist()))
        entry['worst'] = dict(zip(('wear', 'own', 'tint'), entry['worst'].round(3).tolist()))
        entry['metres'] = round(entry['metres'], 1)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    parser.add_argument('--from', dest='source', default='meadow-ground-1',
                        help='staging set with the meadow\'s ground to copy (default: %(default)s)')
    parser.add_argument('--again', action='store_true', help='rebuild the maps of a set this script made')
    parser.add_argument('--layer-size', type=int, default=LAYER_SIZE)
    parser.add_argument('--seed', type=int, default=7)
    parser.add_argument('--crust-screen', type=float, nargs=3, default=CRUST_SCREEN, metavar=('R', 'G', 'B'))
    parser.add_argument('--loose-screen', type=float, nargs=3, default=(1., 1., 1.), metavar=('R', 'G', 'B'),
                        help='of the mean of the meadow\'s earth, which the loose layer is brought to')
    parser.add_argument('--wander', type=float, help='cm, 0..30: the record\'s; KNOBS without')
    parser.add_argument('--fade', type=float, help='0..1')
    parser.add_argument('--ragged', type=float, help='0..2')
    parser.add_argument('--set', nargs='*', default=[], metavar='NAME=VALUE', dest='constants',
                        help='a probe: constants of this file by name')
    parser.add_argument('--only', nargs='*', default=list(ROADS), metavar='TEXTURE',
                        help='the road textures that get `track`; the others keep their `verge` alone')
    parser.add_argument('--preview', type=Path, help='write previews and the composed strip here and stop: no set')
    parser.add_argument('--joints', type=Path, help='a base (~/.cache/gothic-ai/base/<fingerprint>): print the joints')
    args = parser.parse_args()
    constants = override(args.constants)

    source, output = STAGING / args.source, STAGING / args.set
    mine = (output / 'meadow-road.json').exists()
    if output.exists() and not args.preview and not (args.again and mine):
        raise SystemExit(f'{output}: staging set exists; choose a new set name, or --again for a set made here')
    index = json.loads(((output if mine else source) / 'materials/materials.json').read_text())
    record = index.get(f'{MEADOW}.TGA', {})
    if 'ground' not in record:
        raise SystemExit(f'{source}: {MEADOW} has no ground; the road needs it')
    pilot, dds = sibling('megascans_pilot.py'), sibling('test-material.py')
    layer, fringe = sibling('landscape_grass_layer.py'), sibling('landscape_fringe.py')

    # the loose layer is the meadow's earth in tone: read from the set's own earth
    earth = json.loads(((output if mine else source) / 'meadow-ground.json').read_text())['earth']
    loose_tone = np.array(earth['mean'])
    crust = scan_layer(pilot, 'crust', CRUST, CRUST_TILE_M, args.layer_size, crust_tone(), args.crust_screen)
    loose = scan_layer(pilot, 'loose', LOOSE, LOOSE_TILE_M, args.layer_size, loose_tone, args.loose_screen)
    ribbons = {}
    for number, texture in enumerate(ROADS):
        fields = dict(DRAW[texture](RIBBON_SIZE, args.seed + 1000 * number), loose_tone=loose_tone * np.array(args.loose_screen))
        ribbons[texture] = ribbon_maps(fields, METRIC[texture], args.seed + 1000 * number)

    knobs = dict(KNOBS, **{key: getattr(args, key) for key in KNOBS if getattr(args, key) is not None})

    def summary(maps):
        value = maps['albedo']
        mean = value.reshape(-1, 3).mean(axis=0)
        hue, saturation, _ = colorsys.rgb_to_hsv(*mean)
        return dict(size=value.shape[0], mean=mean.round(4).tolist(), luminance=round(float(luma(mean)), 4),
                    hue=round(hue * 360, 1), saturation=round(saturation, 2),
                    luma_std=round(float(luma(value).std() / luma(value).mean()), 3), gains=maps['gains'].round(3).tolist(),
                    slope_rms=round(float(np.sqrt((maps['slope'] ** 2).sum(axis=2).mean())), 3))
    report = dict(source=args.source, seed=args.seed, tracks=list(args.only), knobs=knobs, constants=constants,
                  crust=dict(summary(crust), scan=CRUST, tile_m=CRUST_TILE_M, screen=list(args.crust_screen)),
                  loose=dict(summary(loose), scan=LOOSE, tile_m=LOOSE_TILE_M, screen=list(args.loose_screen)),
                  ribbons={FOLDER[texture]: dict(
                      metric=list(METRIC[texture]), own=round(float(maps['mask'][..., 2].mean()), 3),
                      wear=round(float(maps['mask'][..., 1].mean()), 3), deepest_cm=round(float(-maps['height'].min() * 100), 1),
                      steepest=round(float(np.abs(maps['slope']).max()), 2)) for texture, maps in ribbons.items()})
    if args.joints:
        report['joints'] = joints(args.joints, ribbons)
    if args.preview:
        report['strip'] = preview(args.preview, pilot, crust, loose, ribbons, dict(KNOBS, **knobs))
        print(json.dumps(report, indent=2))
        return 0

    if not mine:
        shutil.copytree(source, output)
    materials = output / 'materials'
    write_layer(materials / 'road_crust', crust, dds, pilot, layer, fringe)
    write_layer(materials / 'road_loose', loose, dds, pilot, layer, fringe)

    def maps_of(folder, tile):
        return dict(albedo=f'{folder}/albedo.dds', normal=f'{folder}/normal.dds', orm=f'{folder}/orm.dds', tile_m=tile)
    for texture in ROADS:
        entry = index.setdefault(f'{texture}.TGA', {})
        entry.pop('track', None)
        folder = FOLDER[texture]
        if texture not in args.only:
            shutil.rmtree(materials / folder, ignore_errors=True)
            continue
        write_ribbon(materials / folder, ribbons[texture], dds, pilot, layer)
        if texture == 'NW_NATURE_PATH_06_END':
            entry['verge'] = dict(entry['verge'], **END_VERGE)
        entry['track'] = dict(crust=maps_of('road_crust', CRUST_TILE_M), loose=maps_of('road_loose', LOOSE_TILE_M),
                              ribbon=dict(tint=f'{folder}/tint.dds', normal=f'{folder}/normal.dds', mask=f'{folder}/mask.dds'),
                              **knobs)
    record['ground'] = dict(record['ground'], border=maps_of('road_loose', LOOSE_TILE_M))
    (materials / 'materials.json').write_text(json.dumps(index, indent=2) + '\n')
    (output / 'meadow-road.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
