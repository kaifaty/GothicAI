#!/usr/bin/env python3
"""Build a staging set where a crop field stands at harvest: strips of rye, of stubble with stooks and of plough.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/crop_field_set.py crop-field-1 \
        [--from ~/.local/share/gothic-ai/content] [--field '106,-139;115.5,-116'] [--preview DIR]

The pilot of reports/content/2026-11-19-crop-fields (intent.md): the two plots of Lobart's main field. Copies a content set and adds,
without touching what it copies:

- one atlas of card pictures, `materials/crop_field/`: rows of rye, stubble and a bound sheaf drawn from numbers
  (mesh3d/blender/crop_cards.py; rendered once into the work folder, `--rerender` to render again). Alpha is the
  texel's coverage in every mip (glb_cards.write_cutout);
- two records of that atlas: `OWN_CROP_RYE` is foliage, the wind leans the stalks about the row's root line (the
  engine's `line` anchor: the roots on the mesh's X); `OWN_CROP_DRY` stands still, for the stooks. Both are
  `"two_sided": "same"`;
- the meshes, `meshes/crop_field/`: a row of rye is three cards 10 cm apart, 1.5 m long, bent in four links, with
  five cards of ears laid over its top every way; a tuft is two crossed cards of a few stalks, for the reaped edge;
  a row of stubble two cards; a stook six to eight sheaves leant into a tent, each sheaf a body of eight sides with
  a fan of three cards of ears;
- the places, `worlds/<WORLD>/vobs.d/crop-field.json`: a field is cut across its long axis into three to five strips
  about 6.5 m wide (rye, stubble, rye, plough, stubble, the next field a strip on; or the mirror of it, whichever
  leaves more of the original's work spots and items in the open); rows of rye stand 30 cm apart along the strip with rows across them, the last EDGE of a strip is
  single tufts that thin out, none within CLEAR of a work spot or an item; the original's cards of grain on the field (NW_NATURE_PLANT_03) are removed.

The ground keeps the original's texture. No levels of detail: a row is 34 triangles.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
HOME = Path.home() / '.local/share/gothic-ai'
STAGING = HOME / 'content-staging'
WORK = HOME / 'crop-fields/work/cards-2'
ATLAS = 2048
DENSITY = 512
FOLDER = 'crop_field'
RYE, DRY = 'OWN_CROP_RYE', 'OWN_CROP_DRY'
FOLIAGE = dict(wind=1.4, flutter=.6, translucency=.45)
NORMAL_UP = .7              # share of straight up in a vertex normal: the field is lit as one surface
ROUGH = .9
AO_ROOT = .55
STRIPS = ('rye', 'stubble', 'rye', 'plough', 'stubble')
ROW_GAP = 30.               # cm between rows of rye along the strip
ROW_STEP = 135.             # cm between rows end to end (a row is 150)
CROSS_GAP = 75.             # cm between the rows across
STUBBLE_GAP = 38.
EDGE = 90.                  # cm of a strip's edge that stands as single tufts, not as rows
TUFTS_M2 = 7.               # tufts a square metre of that edge
CLEAR = 130.                # cm round a work spot or an item where no rye stands
STRIP_WIDTH = 650.          # cm: a field is cut into strips about this wide, three of them at least
STOOK_STEP = 700.           # cm between stooks along a stubble strip
PLANT = ('NW_NATURE_PLANT_03.3DS', 'NW_NATURE_GREAT_WEED_XGROUP.3DS')    # the original's grain: clumps and groups of rows
REACH = 8000.               # cm from the given point within which the field's triangles are looked for
STRIPS_MAX = 12             # a big field (Onar's plots are 55 m across) is cut into more strips of the same width


def sibling(name: str):
    path = HERE / name
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def unit(v):
    v = np.asarray(v, np.float64)
    return v / np.linalg.norm(v)


def render(folder: Path, seed: int) -> None:
    blender = os.environ.get('BLENDER') or shutil.which('blender') or '/Applications/Blender.app/Contents/MacOS/Blender'
    subprocess.run([blender, '-b', '-P', str(HERE / 'mesh3d/blender/crop_cards.py'), '--', '--out', str(folder),
                    '--density', str(DENSITY), '--seed', str(seed)], check=True)


def pack(sizes: dict, side: int) -> dict:
    x = y = shelf = 0
    places = {}
    for name, (width, height) in sorted(sizes.items(), key=lambda item: -item[1][1]):
        width, height = -(-width // 4) * 4, -(-height // 4) * 4
        if x + width > side:
            x, y, shelf = 0, y + shelf, 0
        places[name] = (x, y)
        x, shelf = x + width, max(shelf, height)
    if y + shelf > side:
        raise SystemExit(f'the cards do not fit {side} px')
    return places


def card(mesh, cell: dict, z: float, turn: float = 0., links: int = 4, mirror: bool = False, lean: float = 0.,
         shift: float = 0.) -> None:
    """An upright card along the mesh's X at depth z (cm), turned about Y by `turn` degrees round its middle,
    its top leant by `lean` cm along Z."""
    half, height = cell['width_cm'] / 2, cell['height_cm']
    c, s = math.cos(math.radians(turn)), math.sin(math.radians(turn))
    along, out = np.array((c, 0., -s)), np.array((s, 0., c))
    normal_own = out
    u0, u1 = cell['u'][::-1] if mirror else cell['u']
    rows = []
    for k in range(links + 1):
        t = k / links
        centre = np.array((shift, height * t, z)) + out * lean * t
        v = cell['v_root'] + (cell['v_tip'] - cell['v_root']) * t
        normal = unit(NORMAL_UP * np.array((0., 1., 0.)) + (1 - NORMAL_UP) * normal_own)
        rows.append([(centre - along * half, normal, (u0, v)), (centre + along * half, normal, (u1, v))])
    for low, high in zip(rows, rows[1:]):
        mesh.quad((low[0], low[1], high[1], high[0]), out, both=True)


def tilted(mesh, cell: dict, x: float, y: float, z: float, yaw: float, tilt: float, mirror: bool = False) -> None:
    """A card of ears over the top of a row: from (x, y, z) up and over by `tilt` degrees from the vertical, turned
    about Y by `yaw`."""
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    along, back = np.array((c, 0., -s)), np.array((s, 0., c))
    t = math.radians(tilt)
    rise = np.array((0., math.cos(t), 0.)) + back * math.sin(t)
    out = np.cross(along, rise)
    if out[1] < 0:
        out = -out
    half, height = cell['width_cm'] / 2, cell['height_cm']
    u0, u1 = cell['u'][::-1] if mirror else cell['u']
    normal = unit(NORMAL_UP * np.array((0., 1., 0.)) + (1 - NORMAL_UP) * out)
    foot = np.array((x, y, z))
    mesh.quad([(foot - along * half, normal, (u0, cell['v_root'])), (foot + along * half, normal, (u1, cell['v_root'])),
               (foot + rise * height + along * half, normal, (u1, cell['v_tip'])),
               (foot + rise * height - along * half, normal, (u0, cell['v_tip']))], out, both=True)


SHEAF_BODY = ((0., 17.), (.3, 11.), (.54, 6.5), (.64, 8.5))    # share of the picture's height, radius in cm
SHEAF_SIDES = 8


def sheaf(mesh, cell: dict, foot, top, turn: float, centre) -> None:
    """A sheaf from its foot to its top (cm, the mesh's axes): a body of eight sides from the butts to over the
    band, the straw of the picture's middle wrapped round it, and the fan of ears above as three crossed cards.
    centre: the stook's middle, the normals lean away from it."""
    foot, top = np.asarray(foot, np.float64), np.asarray(top, np.float64)
    axis = unit(top - foot)
    length = np.linalg.norm(top - foot)
    first = unit(np.cross(axis, (0., 1., 0.)))
    second = np.cross(axis, first)
    away = unit((foot - np.asarray(centre)) * (1, 0, 1) + (1e-3, 0, 0))
    middle = (cell['u'][0] + cell['u'][1]) / 2
    per_cm = (cell['u'][1] - cell['u'][0]) / cell['width_cm']
    rings = []
    for share, radius in SHEAF_BODY:
        v = cell['v_root'] + (cell['v_tip'] - cell['v_root']) * share
        ring = []
        for k in range(SHEAF_SIDES + 1):
            angle = 2 * math.pi * k / SHEAF_SIDES + math.radians(turn)
            around = first * math.cos(angle) + second * math.sin(angle)
            # the picture's straw is as wide as the sheaf is there: its middle goes round, there and back
            u = middle + (radius * .8 * per_cm) * (1 if k % 2 else -1)
            ring.append((foot + axis * length * share + around * radius,
                         unit(.55 * around + .25 * away + .2 * np.array((0., 1., 0.))), (u, v)))
        rings.append(ring)
    for low, high in zip(rings, rings[1:]):
        for k in range(SHEAF_SIDES):
            face = unit(low[k][1] + low[k + 1][1])
            mesh.quad((low[k], low[k + 1], high[k + 1], high[k]), face)
    start = .5
    half = cell['width_cm'] / 2
    v0 = cell['v_root'] + (cell['v_tip'] - cell['v_root']) * start
    for k in range(3):
        angle = math.pi * k / 3 + math.radians(turn)
        across = first * math.cos(angle) + second * math.sin(angle)
        out = np.cross(across, axis)
        normal = unit(.5 * np.array((0., 1., 0.)) + .5 * away)
        low_at, high_at = foot + axis * length * start, foot + axis * length
        mesh.quad([(low_at - across * half, normal, (cell['u'][0], v0)), (low_at + across * half, normal, (cell['u'][1], v0)),
                   (high_at + across * half, normal, (cell['u'][1], cell['v_tip'])),
                   (high_at - across * half, normal, (cell['u'][0], cell['v_tip']))], out, both=True)


def build_meshes(cards, cells: dict, directory: Path, rng) -> dict:
    """name -> dict(glb, triangles, material)."""
    made = {}

    def write(name, mesh, material):
        mesh.write(directory / f'{name}.glb', material, group='UNDEF')
        made[name] = dict(glb=f'{FOLDER}/{name}.glb', triangles=mesh.triangles, material=material)

    ear_cards = ('ears_a', 'ears_b')
    for name, (a, b, c), flip in (('A', ('rye_a', 'rye_b', 'rye_c'), (False, True, False)),
                                  ('B', ('rye_c', 'rye_a', 'rye_b'), (True, False, True)),
                                  ('C', ('rye_b', 'rye_c', 'rye_a'), (False, False, True))):
        mesh = cards.Mesh()
        card(mesh, cells[a], -10., turn=-5., mirror=flip[0], lean=-3.)
        card(mesh, cells[b], 0., turn=3., mirror=flip[1], shift=6.)
        card(mesh, cells[c], 10., turn=7., mirror=flip[2], lean=4., shift=-5.)
        # the top: ears laid over the row every way, so that from above it is ears and not the cards' edges
        for k in range(5):
            tilted(mesh, cells[ear_cards[k % 2]], rng.uniform(-55, 55), rng.uniform(100, 116), rng.uniform(-16, 16),
                   rng.uniform(0, 360), rng.uniform(42, 68), mirror=bool(k % 2))
        write(f'{RYE}_ROW_{name}', mesh, RYE)
    for name, (a, b) in (('A', ('tuft_a', 'tuft_c')), ('B', ('tuft_b', 'tuft_d')), ('C', ('tuft_d', 'tuft_a')),
                         ('D', ('tuft_c', 'tuft_b'))):
        mesh = cards.Mesh()                     # single stalks: the reaped edge of a strip, a straggler in the stubble
        turn = rng.uniform(0, 180)
        card(mesh, cells[a], 0., turn=turn, lean=rng.uniform(-6, 6))
        card(mesh, cells[b], 0., turn=turn + rng.uniform(60, 120), mirror=True, lean=rng.uniform(-6, 6))
        write(f'{RYE}_TUFT_{name}', mesh, RYE)
    for name, (a, b) in (('A', ('stubble_a', 'stubble_b')), ('B', ('stubble_b', 'stubble_a'))):
        mesh = cards.Mesh()
        card(mesh, cells[a], -8., turn=-7., links=1, lean=-3.)
        card(mesh, cells[b], 8., turn=6., links=1, mirror=True, lean=3.)
        write(f'{RYE}_STUBBLE_{name}', mesh, RYE)
    for name, count in (('A', 7), ('B', 6), ('C', 8)):
        mesh = cards.Mesh()
        height = cells['sheaf']['height_cm']
        for k in range(count):
            angle = 2 * math.pi * (k + rng.uniform(-.2, .2)) / count
            radius = rng.uniform(40, 52)
            foot = np.array((math.cos(angle) * radius, 0., math.sin(angle) * radius))
            reach = rng.uniform(.94, 1.)
            tilt = math.asin(min((radius - rng.uniform(6, 12)) / (height * reach), .9))
            top = foot * (1 - math.sin(tilt) * height * reach / radius) + (0., math.cos(tilt) * height * reach, 0.)
            sheaf(mesh, cells['sheaf'], foot, top, rng.uniform(0, 45), (0., 0., 0.))
        write(f'{DRY}_STOOK_{name}', mesh, DRY)
    return made


class Ground:
    """The field's triangles in the game's axes, cm: the height and the normal under a point."""

    def __init__(self, triangles: np.ndarray):
        self.t = triangles
        a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
        self.normal = np.cross(b - a, c - a)
        self.normal *= np.sign(self.normal[:, 1:2])
        self.normal /= np.linalg.norm(self.normal, axis=1, keepdims=True)
        self.area = np.linalg.norm(np.cross((b - a) * (1, 0, 1), (c - a) * (1, 0, 1)), axis=1) / 2

    def at(self, x: float, z: float):
        a, b, c = self.t[:, 0], self.t[:, 1], self.t[:, 2]
        d = (b[:, 2] - c[:, 2]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 2] - c[:, 2])
        d = np.where(np.abs(d) < 1e-9, 1e-9, d)
        w0 = ((b[:, 2] - c[:, 2]) * (x - c[:, 0]) + (c[:, 0] - b[:, 0]) * (z - c[:, 2])) / d
        w1 = ((c[:, 2] - a[:, 2]) * (x - c[:, 0]) + (a[:, 0] - c[:, 0]) * (z - c[:, 2])) / d
        w2 = 1 - w0 - w1
        inside = np.flatnonzero((w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4))
        if not len(inside):
            return None
        heights = w0[inside] * a[inside, 1] + w1[inside] * b[inside, 1] + w2[inside] * c[inside, 1]
        k = inside[np.argmax(heights)]
        return float(heights.max()), self.normal[k]


def walk(node, out):
    if isinstance(node, dict):
        if 'position' in node:
            out.append(node)
        for value in node.values():
            walk(value, out)
    elif isinstance(node, list):
        for value in node:
            walk(value, out)


def place(base: Path, world: str, field, meshes: dict, seed: int, number: int = 0):
    field_number = number
    census = sibling('world/world_census.py')
    mesh = census.load_world(base, world, None, None)
    names = np.array([m.get('texture') or m['name'] for m in mesh.materials])[mesh.material]
    triangles = mesh.positions * (-100., 100., 100.)                # the mesh mirrors x against the game's axes
    centre = triangles.mean(axis=1)
    acker = np.array(['ACKER' in n for n in names])
    near = acker & (np.hypot(centre[:, 0] - field[0] * 100, centre[:, 2] - field[1] * 100) < REACH)
    # the field is the patch of tilled triangles joined by shared corners to the one nearest the given point
    index = np.flatnonzero(near)
    corners = np.rint(triangles[index].reshape(-1, 3) / 5).astype(np.int64)
    _, label = np.unique(corners, axis=0, return_inverse=True)
    label = label.reshape(-1, 3)
    parent = np.arange(label.max() + 1)

    def find(k):
        while parent[k] != k:
            parent[k] = parent[parent[k]]
            k = parent[k]
        return k

    for a, b, c in label:
        parent[find(a)] = parent[find(b)] = find(c)
    group = np.array([find(a) for a in label[:, 0]])
    seed_triangle = np.argmin(np.hypot(centre[index, 0] - field[0] * 100, centre[index, 2] - field[1] * 100))
    ground = Ground(triangles[index[group == group[seed_triangle]]])
    area = float(ground.area.sum()) / 1e4

    # the field's axes: u its long one, v across; the strips are cut along v
    points = ground.t.reshape(-1, 3)[:, (0, 2)]
    middle = (ground.t.mean(axis=1)[:, (0, 2)] * ground.area[:, None]).sum(axis=0) / ground.area.sum()
    _, _, axes = np.linalg.svd((ground.t.mean(axis=1)[:, (0, 2)] - middle) * np.sqrt(ground.area)[:, None],
                               full_matrices=False)
    u_axis, v_axis = axes[0], axes[1]
    u_all, v_all = (points - middle) @ u_axis, (points - middle) @ v_axis
    v0, v1 = v_all.min(), v_all.max()
    count = int(np.clip(round((v1 - v0) / STRIP_WIDTH), 3, STRIPS_MAX))
    pattern = tuple(STRIPS[(number + k) % len(STRIPS)] for k in range(count))    # the next field starts a strip on
    width = (v1 - v0) / count

    vobs = []
    walk(json.loads((base / 'worlds' / world / 'vobs.json').read_text()), vobs)
    keep, remove = [], []
    for vob in vobs:
        x, _, z = vob['position']
        if ground.at(x, z) is None:
            continue
        name = str(vob.get('vob_name') or '')
        if vob.get('visual_name') in PLANT:
            remove.append(int(vob['id']))
        elif name.startswith('FP_') or vob.get('type') == 'oCItem':
            keep.append((x, z))
    keep = np.array(keep).reshape(-1, 2)

    def strip_of(order, v):
        return order[int(np.clip((v - v0) / width, 0, len(order) - 1))]

    def in_open(order):
        return sum(strip_of(order, (p - middle) @ v_axis) != 'rye' for p in keep)

    order = max((pattern, pattern[::-1]), key=in_open)
    rng = np.random.default_rng(seed + number)
    added, counts = [], {}

    def add(visual, x, z, along, jitter=8., sink=2.):
        """A row at (x, z) with its X along `along` (a unit vector of the world's x and z), laid on the ground."""
        hit = ground.at(x, z)
        if hit is None:
            return False
        ahead, back = ground.at(x + along[0] * 60, z + along[1] * 60), ground.at(x - along[0] * 60, z - along[1] * 60)
        if ahead is None or back is None:
            return False
        if rng.random() < .5:
            along = -np.asarray(along)
        turn = math.radians(rng.normal(0, jitter))
        along = np.array((along[0] * math.cos(turn) - along[1] * math.sin(turn),
                          along[0] * math.sin(turn) + along[1] * math.cos(turn)))
        slope = (ground.at(x + along[0] * 60, z + along[1] * 60) or hit)[0] - (ground.at(x - along[0] * 60, z - along[1] * 60) or hit)[0]
        side = unit((along[0] * 120, slope, along[1] * 120))
        out = unit(np.cross(side, (0., 1., 0.)))
        up = np.cross(out, side)
        rotation = np.stack([side, up, out], axis=1)
        added.append(dict(key=f'crop-{field_number + 1}-{len(added) + 1:05d}', kind='mesh', visual=visual,
                          position=[round(x, 1), round(hit[0] - sink, 1), round(z, 1)],
                          rotation=[[round(float(c), 5) for c in row] for row in rotation],
                          collision='mesh' if '_STOOK_' in visual else 'none'))    # a stook stands in the way, the rye does not
        counts[visual] = counts.get(visual, 0) + 1
        return True

    def world(u, v):
        return middle + u_axis * u + v_axis * v

    def clear(p, radius=CLEAR):
        return not len(keep) or np.hypot(*(keep - p).T).min() > radius

    rows = [f'{RYE}_ROW_{k}' for k in 'ABC']
    u0, u1 = u_all.min(), u_all.max()
    for strip, kind in enumerate(order):
        low, high = v0 + strip * width, v0 + (strip + 1) * width
        if kind == 'rye':
            # the strip's edge wanders by a slow noise, so the reaped line is not ruled
            phase = rng.uniform(0, 6.28, 4)
            wander = lambda u, k: 70 * math.sin(u / 310 + phase[k]) + 40 * math.sin(u / 97 + phase[k + 2])
            for v in np.arange(low, high, ROW_GAP):
                for u in np.arange(u0 + rng.uniform(0, ROW_STEP), u1, ROW_STEP):
                    uu, vv = u + rng.normal(0, 12), v + rng.normal(0, 5)
                    if not low + 20 + wander(uu, 0) < vv < high - 20 + wander(uu, 1):
                        continue
                    p = world(uu, vv)
                    if clear(p):
                        edge = vv - low - wander(uu, 0) < EDGE or high + wander(uu, 1) - vv < EDGE
                        if not edge:
                            add(rows[rng.integers(3)], *p, u_axis, sink=rng.uniform(2, 12))
            for u in np.arange(u0, u1, CROSS_GAP):
                for v in np.arange(low + 75, high - 60, ROW_STEP):
                    uu, vv = u + rng.normal(0, 10), v + rng.normal(0, 15)
                    p = world(uu, vv)
                    if clear(p) and low + EDGE + 60 + wander(uu, 0) < vv < high - EDGE - 60 + wander(uu, 1):
                        add(rows[rng.integers(3)], *p, v_axis, jitter=18., sink=rng.uniform(2, 12))
            # the reaped edge: single tufts, thinner outwards, some lower, a few left standing in the stubble
            for side, sign in ((0, 1.), (1, -1.)):
                for _ in range(round((u1 - u0) / 100 * (EDGE + 60) / 100 * TUFTS_M2)):
                    uu = rng.uniform(u0, u1)
                    inward = rng.uniform(-60, EDGE) ** 1.0
                    if inward < 0 and rng.random() < .75:      # few of them outside the line
                        continue
                    if rng.random() > .35 + .65 * np.clip(inward / EDGE, 0, 1):
                        continue
                    vv = (low if side == 0 else high) + wander(uu, side) + sign * (20 + inward)
                    p = world(uu, vv)
                    if clear(p):
                        add(f'{RYE}_TUFT_{"ABCD"[rng.integers(4)]}', *p, unit(rng.normal(0, 1, 2)), jitter=180.,
                            sink=rng.uniform(2, 14) + (rng.uniform(10, 45) if rng.random() < .2 else 0.))
        elif kind == 'stubble':
            for v in np.arange(low + 10, high - 10, STUBBLE_GAP):
                for u in np.arange(u0 + rng.uniform(0, ROW_STEP), u1, ROW_STEP):
                    if rng.random() < .2:                       # a trodden gap
                        continue
                    p = world(u + rng.normal(0, 30), v + rng.normal(0, 14))
                    add(f'{RYE}_STUBBLE_{"AB"[rng.integers(2)]}', *p, u_axis, jitter=28., sink=rng.uniform(1, 6))
            for u in np.arange(u0 + 250, u1 - 150, STOOK_STEP):
                p = world(u + rng.normal(0, 120), (low + high) / 2 + rng.normal(0, width * .14))
                if clear(p, 200):
                    add(f'{DRY}_STOOK_{"ABC"[rng.integers(3)]}', *p, unit(rng.normal(0, 1, 2)), jitter=180.)
    stats = dict(field_m2=round(area, 1), strips=list(order), strip_width_cm=round(float(width), 1),
                 long_axis=[round(float(c), 4) for c in u_axis], length_cm=round(float(u1 - u0), 1),
                 work_spots_and_items=len(keep), of_them_in_open=in_open(order), removed_plants=len(remove),
                 added=len(added), by_visual=counts,
                 triangles=sum(meshes[v]['triangles'] * n for v, n in counts.items()))
    return added, remove, stats


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    parser.add_argument('--from', dest='source', type=Path, default=HOME / 'content')
    parser.add_argument('--field', default='106,-139;115.5,-116',
                        help='x,z of a point of each field, metres of the waynet, `;` between fields')
    parser.add_argument('--world', default='NEWWORLD')
    parser.add_argument('--base', type=Path)
    parser.add_argument('--cards', type=Path, default=WORK)
    parser.add_argument('--rerender', action='store_true')
    parser.add_argument('--seed', type=int, default=7)
    parser.add_argument('--gain', type=float, default=1., help='the atlas brighter or darker')
    parser.add_argument('--preview', type=Path, help='write the atlas as PNG here and stop: no set')
    args = parser.parse_args()
    cards, dds, pilot = sibling('glb_cards.py'), sibling('test-material.py'), sibling('megascans_pilot.py')

    output = STAGING / args.set
    if output.exists() and not args.preview:
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    if args.rerender or not (args.cards / 'cards.npz').exists():
        render(args.cards, args.seed)
    listing = json.loads((args.cards / 'cards.json').read_text())
    passes = np.load(args.cards / 'cards.npz')
    margin = round(listing['margin_m'] * DENSITY)
    places = pack({c['name']: c['px'] for c in listing['cards']}, ATLAS)
    colour, alpha, rise = np.zeros((ATLAS, ATLAS, 3)), np.zeros((ATLAS, ATLAS)), np.ones((ATLAS, ATLAS))
    cells = {}
    for c in listing['cards']:
        name, (width, height), (x, y) = c['name'], c['px'], places[c['name']]
        picture = passes[name].astype(np.float64)
        colour[y:y + height, x:x + width] = picture[..., :3]
        alpha[y:y + height, x:x + width] = picture[..., 3]
        rise[y:y + height, x:x + width] = np.linspace(1, 0, height)[:, None]
        cells[name] = dict(u=(x / ATLAS, (x + width) / ATLAS), v_root=(y + height - margin) / ATLAS, v_tip=y / ATLAS,
                           width_cm=c['width_m'] * 100, height_cm=(c['height_m'] - listing['margin_m']) * 100)
    colour = np.clip(colour * args.gain, 0, 1)
    weight = alpha[..., None]
    mean = (colour * weight).sum(axis=(0, 1)) / weight.sum()
    if args.preview:
        from PIL import Image
        args.preview.mkdir(parents=True, exist_ok=True)
        Image.fromarray(np.dstack([pilot.encode(cards.spread_colour(colour, alpha)),
                                   np.rint(alpha * 255).astype(np.uint8)])).save(args.preview / 'atlas.png')
        print(json.dumps(dict(mean=mean.round(4).tolist(), cells=cells), indent=2))
        return 0

    work = output.with_name(output.name + '.tmp')
    if work.exists():
        shutil.rmtree(work)
    subprocess.run(['cp', '-Rc', str(args.source), str(work)], check=True)     # an APFS clone: no bytes copied
    rng = np.random.default_rng(args.seed)
    meshes = build_meshes(cards, cells, work / 'meshes' / FOLDER, rng)
    index_path = work / 'meshes/meshes.json'
    index = json.loads(index_path.read_text()) if index_path.exists() else {}
    index_path.write_text(json.dumps(index | {name: dict(glb=m['glb']) for name, m in meshes.items()}, indent=2) + '\n')

    directory = work / 'materials' / FOLDER
    directory.mkdir(parents=True)
    coverage = cards.write_cutout(dds, pilot.encode, directory / 'albedo.dds', colour, alpha)
    small = ATLAS // 512
    ao = cards.spread_colour((AO_ROOT + (1 - AO_ROOT) * rise)[..., None], alpha)[..., 0]
    ao = ao.reshape(512, small, 512, small).mean(axis=(1, 3))
    dds.write_dds(directory / 'orm.dds', np.stack([ao, np.full_like(ao, ROUGH), np.zeros_like(ao)], axis=2).astype(np.float32))
    table = work / 'materials/materials.json'
    materials = json.loads(table.read_text())
    record = dict(albedo=f'{FOLDER}/albedo.dds', orm=f'{FOLDER}/orm.dds', two_sided='same')
    materials[f'{RYE}.TGA'] = record | dict(foliage=FOLIAGE)
    materials[f'{DRY}.TGA'] = dict(record)
    table.write_text(json.dumps(materials, indent=2) + '\n')

    base = args.base or sibling('world/world_census.py').resolve_base(None)
    added, removed, stats = [], [], []
    for number, field in enumerate(args.field.split(';')):
        more, gone, numbers = place(base, args.world, tuple(map(float, field.split(','))), meshes, args.seed, number)
        added, removed, stats = added + more, removed + gone, stats + [numbers]
    layer = work / 'worlds' / args.world / 'vobs.d/crop-field.json'
    layer.parent.mkdir(parents=True, exist_ok=True)
    layer.write_text(json.dumps(dict(format=1, add=added, remove=removed), separators=(',', ':')) + '\n')
    report = dict(source=str(args.source), field=args.field, seed=args.seed, atlas_px=ATLAS, density_px_m=DENSITY,
                  gain=args.gain, mean=mean.round(4).tolist(), coverage_by_mip=[round(c, 4) for c in coverage],
                  normal_up=NORMAL_UP, foliage=FOLIAGE, meshes=meshes, placement=stats)
    (work / 'crop-field.json').write_text(json.dumps(report, indent=2) + '\n')
    work.rename(output)
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
