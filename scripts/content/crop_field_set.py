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

Since step A of alive.md (crop_field_life.py holds its numbers and maps) the field is grown and worked, not laid out:

- the strips are 5 to 9 m wide and bend along the field as a shallow S, the rows follow the bend;
- the rye stands lower by patches of 5 to 10 m (the rows sink up to 30 cm), in three tones of one atlas by patches
  (`OWN_CROP_RYE`, `_PALE`, `_GREEN`: three colour maps, one layout), lies over in lodged spots and is missing in a
  few bare ones; the rows at a strip's edge are lit more by their own normals (NORMAL_EDGE), a wall, not a top;
- weeds, an atlas of their own (`OWN_CROP_WEED`): cornflower, corncockle and mayweed at the edges of the rye and in
  its bare spots, thistle and mayweed along the field's border;
- one strip of rye is being reaped: a ragged front across it, loose swaths behind the front, bound sheaves lying
  further, then stooks, a cart with sheaves, a rake and a sickle;
- every strip has a ground of its own, a mesh over the field's triangles: ridges of plough, straw litter under
  the stubble, earth under the rye (`OWN_CROP_EARTH`, `OWN_CROP_LITTER`).

No levels of detail: a row is 34 triangles.
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
WORK = HOME / 'crop-fields/work/cards-3'
ATLAS = 2048
DENSITY = 512
FOLDER = 'crop_field'
RYE, DRY, WEED = 'OWN_CROP_RYE', 'OWN_CROP_DRY', 'OWN_CROP_WEED'
EARTH, LITTER = 'OWN_CROP_EARTH', 'OWN_CROP_LITTER'
TONES = ('', 'PALE', 'GREEN')
WEED_ATLAS = 1024
WEED_KINDS = ('thistle', 'cockle', 'cornflower', 'mayweed')
FOLIAGE = dict(wind=1.4, flutter=.6, translucency=.45)
NORMAL_UP = .7              # share of straight up in a vertex normal: the field is lit as one surface
NORMAL_EDGE = .4            # of the rows and tufts at a strip's edge: the wall of rye is lit as a wall
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
STRIP_SPREAD = (.72, 1.32)  # a strip is this much of the mean width: 5 to 9 m
BEND = (140., 230.)         # cm the strips swing to either side along the field, a shallow S
FRONT = (.4, .6)            # where along the strip being reaped the reapers are
SWATHS, SHEAVES = 350., 1000.   # cm behind the front: loose swaths to here, lying sheaves to here, stooks beyond
CART = 'NW_CITY_CART_01.3DS'
RAKE, SICKLE = 'ITMI_RAKE.3DS', 'ITMW_007_1H_SICKLE_01.3DS'
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


def tone_material(tone: str) -> str:
    return RYE + ('_' + tone if tone else '')


def card(mesh, cell: dict, z: float, turn: float = 0., links: int = 4, mirror: bool = False, lean: float = 0.,
         shift: float = 0., up: float = NORMAL_UP) -> None:
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
        normal = unit(up * np.array((0., 1., 0.)) + (1 - up) * normal_own)
        rows.append([(centre - along * half, normal, (u0, v)), (centre + along * half, normal, (u1, v))])
    for low, high in zip(rows, rows[1:]):
        mesh.quad((low[0], low[1], high[1], high[0]), out, both=True)


def tilted(mesh, cell: dict, x: float, y: float, z: float, yaw: float, tilt: float, mirror: bool = False,
           up: float = NORMAL_UP) -> None:
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
    normal = unit(up * np.array((0., 1., 0.)) + (1 - up) * out)
    foot = np.array((x, y, z))
    mesh.quad([(foot - along * half, normal, (u0, cell['v_root'])), (foot + along * half, normal, (u1, cell['v_root'])),
               (foot + rise * height + along * half, normal, (u1, cell['v_tip'])),
               (foot + rise * height - along * half, normal, (u0, cell['v_tip']))], out, both=True)


# share of the picture's height, radius in cm: thinner than the picture's sheaf, whose loose straws stand out of it
SHEAF_BODY = ((0., 12.5), (.3, 8.), (.54, 5.), (.64, 6.))
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
    half = cell['width_cm'] / 2
    for k in range(3):
        # the cards run the sheaf's whole length: loose straws stand out of the body's outline
        start = 0.
        v0 = cell['v_root'] + (cell['v_tip'] - cell['v_root']) * start
        angle = math.pi * k / 3 + math.radians(turn)
        across = first * math.cos(angle) + second * math.sin(angle)
        out = np.cross(across, axis)
        normal = unit(.5 * np.array((0., 1., 0.)) + .5 * away)
        low_at, high_at = foot + axis * length * start, foot + axis * length
        mesh.quad([(low_at - across * half, normal, (cell['u'][0], v0)), (low_at + across * half, normal, (cell['u'][1], v0)),
                   (high_at + across * half, normal, (cell['u'][1], cell['v_tip'])),
                   (high_at - across * half, normal, (cell['u'][0], cell['v_tip']))], out, both=True)


def build_meshes(cards, cells: dict, weeds: dict, directory: Path, rng) -> dict:
    """name -> dict(glb, triangles, material)."""
    made = {}

    def write(name, mesh, material):
        mesh.write(directory / f'{name}.glb', material, group='UNDEF')
        made[name] = dict(glb=f'{FOLDER}/{name}.glb', triangles=mesh.triangles, material=material)

    ear_cards = ('ears_a', 'ears_b')
    for name, (a, b, c), flip in (('A', ('rye_a', 'rye_b', 'rye_c'), (False, True, False)),
                                  ('B', ('rye_c', 'rye_a', 'rye_b'), (True, False, True)),
                                  ('C', ('rye_b', 'rye_c', 'rye_a'), (False, False, True))):
        tops = [(rng.uniform(-55, 55), rng.uniform(100, 116), rng.uniform(-16, 16), rng.uniform(0, 360),
                 rng.uniform(42, 68)) for _ in range(5)]
        for kind, up in (('ROW', NORMAL_UP), ('ROWE', NORMAL_EDGE)):
            mesh = cards.Mesh()
            card(mesh, cells[a], -10., turn=-5., mirror=flip[0], lean=-3., up=up)
            card(mesh, cells[b], 0., turn=3., mirror=flip[1], shift=6., up=up)
            card(mesh, cells[c], 10., turn=7., mirror=flip[2], lean=4., shift=-5., up=up)
            # the top: ears laid over the row every way, so that from above it is ears and not the cards' edges
            for k, top in enumerate(tops):
                tilted(mesh, cells[ear_cards[k % 2]], *top, mirror=bool(k % 2))
            for tone in TONES:
                write(f'{tone_material(tone)}_{kind}_{name}', mesh, tone_material(tone))
    for name, (a, b) in (('A', ('tuft_a', 'tuft_c')), ('B', ('tuft_b', 'tuft_d')), ('C', ('tuft_d', 'tuft_a')),
                         ('D', ('tuft_c', 'tuft_b'))):
        mesh = cards.Mesh()                     # single stalks: the reaped edge of a strip, a straggler in the stubble
        turn = rng.uniform(0, 180)
        card(mesh, cells[a], 0., turn=turn, lean=rng.uniform(-6, 6), up=NORMAL_EDGE)
        card(mesh, cells[b], 0., turn=turn + rng.uniform(60, 120), mirror=True, lean=rng.uniform(-6, 6), up=NORMAL_EDGE)
        for tone in TONES:
            write(f'{tone_material(tone)}_TUFT_{name}', mesh, tone_material(tone))
    for name, (a, b) in (('A', ('stubble_a', 'stubble_b')), ('B', ('stubble_b', 'stubble_a'))):
        mesh = cards.Mesh()
        card(mesh, cells[a], -8., turn=-7., links=1, lean=-3.)
        card(mesh, cells[b], 8., turn=6., links=1, mirror=True, lean=3.)
        write(f'{RYE}_STUBBLE_{name}', mesh, RYE)
    height = cells['sheaf']['height_cm']
    for name, count in (('A', 7), ('B', 6), ('C', 8)):
        mesh = cards.Mesh()
        for k in range(count):
            angle = 2 * math.pi * (k + rng.uniform(-.2, .2)) / count
            radius = rng.uniform(40, 52)
            foot = np.array((math.cos(angle) * radius, 0., math.sin(angle) * radius))
            reach = rng.uniform(.94, 1.)
            tilt = math.asin(min((radius - rng.uniform(6, 12)) / (height * reach), .9))
            top = foot * (1 - math.sin(tilt) * height * reach / radius) + (0., math.cos(tilt) * height * reach, 0.)
            sheaf(mesh, cells['sheaf'], foot, top, rng.uniform(0, 45), (0., 0., 0.))
        write(f'{DRY}_STOOK_{name}', mesh, DRY)
    for name, lying in (('A', ((0., 0., 4.),)), ('B', ((-8., -24., -6.), (10., 26., 9.)))):
        mesh = cards.Mesh()                     # bound sheaves lying where they were tied: along the mesh's X
        for shift, z, turn in lying:
            c, s = math.cos(math.radians(turn)), math.sin(math.radians(turn))
            way = np.array((c, 0., s))
            middle = np.array((shift, 15., z))
            sheaf(mesh, cells['sheaf'], middle - way * height / 2, middle + way * height / 2 + (0., 3., 0.),
                  rng.uniform(0, 45), middle - (0., 200., 0.))
        write(f'{DRY}_SHEAF_{name}', mesh, DRY)
    for name, lay in (('A', (('rye_a', -6., 84.), ('rye_c', 9., 87.))), ('B', (('rye_b', 7., 86.), ('rye_a', -11., 82.),
                                                                              ('tuft_b', 30., 80.)))):
        mesh = cards.Mesh()                     # a swath: cut rye laid down unbound, the stalks along the mesh's Z
        for k, (cell, yaw, tilt) in enumerate(lay):
            tilted(mesh, cells[cell], rng.uniform(-12, 12), 3. + 3 * k, -80. + rng.uniform(-10, 10), yaw, tilt,
                   mirror=bool(k % 2), up=.85)
        write(f'{DRY}_SWATH_{name}', mesh, DRY)
    for name, cell in (('THISTLE', 'thistle'), ('COCKLE', 'cockle'), ('CORNFLOWER_A', 'cornflower_a'),
                       ('CORNFLOWER_B', 'cornflower_b'), ('MAYWEED', 'mayweed')):
        mesh = cards.Mesh()
        turn = rng.uniform(0, 180)
        card(mesh, weeds[cell], 0., turn=turn, links=3, lean=rng.uniform(-5, 5), up=.5)
        card(mesh, weeds[cell], 0., turn=turn + rng.uniform(70, 110), links=3, mirror=True, lean=rng.uniform(-5, 5), up=.5)
        write(f'{WEED}_{name}', mesh, WEED)
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


def place(base: Path, world: str, field, meshes: dict, seed: int, number: int, life, ground_mesh):
    """ground_mesh(mesh, material) -> the visual's name: writes the mesh of a piece of a strip's ground."""
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
    rng = np.random.default_rng(seed + number)

    # the field's axes: u its long one, v across. The strips run along u and swing across it as a shallow S, the
    # way a long plough team left them: w is v less the swing, a strip is a band of w
    points = ground.t.reshape(-1, 3)[:, (0, 2)]
    middle = (ground.t.mean(axis=1)[:, (0, 2)] * ground.area[:, None]).sum(axis=0) / ground.area.sum()
    _, _, axes = np.linalg.svd((ground.t.mean(axis=1)[:, (0, 2)] - middle) * np.sqrt(ground.area)[:, None],
                               full_matrices=False)
    u_axis, v_axis = axes[0], axes[1]
    u_all, v_all = (points - middle) @ u_axis, (points - middle) @ v_axis
    u0, u1 = float(u_all.min()), float(u_all.max())
    swing, turn = rng.uniform(*BEND) * rng.choice((-1., 1.)), rng.uniform(0, 2 * math.pi)

    def bend(u):
        return swing * np.sin(2 * math.pi * (u - u0) / (u1 - u0)) + 45 * np.sin(u / 620 + turn)

    def tangent(u):
        slope = swing * 2 * math.pi / (u1 - u0) * math.cos(2 * math.pi * (u - u0) / (u1 - u0)) + 45 / 620 * math.cos(u / 620 + turn)
        return unit(u_axis + v_axis * slope)

    def across(u):
        t = tangent(u)
        return np.array((-t[1], t[0])) * np.sign(np.dot((-t[1], t[0]), v_axis))

    def spot(u, w):
        return middle + u_axis * u + v_axis * (w + float(bend(u)))

    w_all = v_all - bend(u_all)
    w0, w1 = float(w_all.min()), float(w_all.max())
    count = int(np.clip(round((w1 - w0) / STRIP_WIDTH), 3, STRIPS_MAX))
    shares = rng.uniform(*STRIP_SPREAD, count)
    bounds = w0 + np.concatenate([[0.], np.cumsum(shares)]) / shares.sum() * (w1 - w0)
    pattern = tuple(STRIPS[(number + k) % len(STRIPS)] for k in range(count))    # the next field starts a strip on

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

    def strip_at(p):
        u = float((p - middle) @ u_axis)
        w = float((p - middle) @ v_axis) - float(bend(u))
        return int(np.clip(np.searchsorted(bounds, w) - 1, 0, count - 1))

    def in_open(order):
        return sum(order[strip_at(p)] != 'rye' for p in keep)

    order = list(max((pattern, pattern[::-1]), key=in_open))
    # one strip of rye is being reaped: of those with stubble beside them, the one that runs the longest over the
    # field (a field is no rectangle: a strip at its side may be a corner); the reapers are in the middle of its run
    def run(k):
        return [u for u in np.arange(u0, u1, 100.) if ground.at(*spot(u, (bounds[k] + bounds[k + 1]) / 2)) is not None]

    beside = [k for k, kind in enumerate(order) if kind == 'rye'
              and 'stubble' in (order[k - 1] if k else '', order[k + 1] if k + 1 < count else '')]
    reaping = max(beside, key=lambda k: len(run(k)), default=None)
    front_u, front_way = (u0 + u1) / 2, rng.choice((-1., 1.))
    if reaping is not None:
        order[reaping] = 'reaping'
        covered = run(reaping)
        front_u = covered[int(len(covered) * rng.uniform(*FRONT))]
    wave = rng.uniform(0, 2 * math.pi, (count + 1, 2))
    ragged = rng.uniform(0, 2 * math.pi, 2)

    def edge(k):
        """The edge between strips k-1 and k, cm across the field at u: it wanders, the reaped line is not ruled."""
        return lambda u: bounds[k] + 70 * math.sin(u / 310 + wave[k, 0]) + 40 * math.sin(u / 97 + wave[k, 1])

    def behind(u, w):
        """cm behind the reapers' front (reaped ground); negative in the standing rye."""
        return front_way * (u - front_u - 110 * math.sin(w / 160 + ragged[0]) - 50 * math.sin(w / 53 + ragged[1]))

    low, high = points.min(axis=0), points.max(axis=0)
    patches = life.Patches(rng, low, high, area * 1e4)
    added, counts = [], {}

    def record(visual, position, rotation, collision='none'):
        added.append(dict(key=f'crop-{field_number + 1}-{len(added) + 1:05d}', kind='mesh', visual=visual,
                          position=[round(float(c), 1) for c in position],
                          rotation=[[round(float(c), 5) for c in row] for row in rotation], collision=collision))
        counts[visual] = counts.get(visual, 0) + 1

    def add(visual, x, z, along, jitter=8., sink=2., lean=None, collision='none'):
        """A row at (x, z) with its X along `along` (a unit vector of the world's x and z), laid on the ground.
        lean: (a unit vector of x and z, radians): the row's top goes over that way."""
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
        if lean is not None:
            way, angle = lean
            sign = 1. if out[0] * way[0] + out[2] * way[1] >= 0 else -1.
            up = up * math.cos(angle) + out * sign * math.sin(angle)
            out = np.cross(side, up)
        record(visual, (x, hit[0] - sink, z), np.stack([side, up, out], axis=1), collision)
        return True

    def clear(p, radius=CLEAR):
        return not len(keep) or np.hypot(*(keep - p).T).min() > radius

    def rye(kind, letters, p, along, jitter=8., sink=(1., 7.), low_share=1.):
        """A row or a tuft of rye: its tone and height by the patch it stands in, lying over in a lodged spot."""
        visual = f'{tone_material(patches.tone_of(p, rng))}_{kind}_{letters[rng.integers(len(letters))]}'
        lodged = patches.lodge(p)
        if lodged is not None:
            way, angle = lodged
            return add(visual, *p, np.array((-way[1], way[0])), jitter=16., sink=rng.uniform(2, 8),
                       lean=(way, angle * rng.uniform(.8, 1.1)))
        return add(visual, *p, along, jitter=jitter, sink=rng.uniform(*sink) + patches.sink(p) * low_share)

    def weed(p, choice):
        visual = f'{WEED}_{choice[rng.integers(len(choice))]}'
        return add(visual, *p, unit(rng.normal(0, 1, 2)), jitter=180., sink=rng.uniform(0, 6))

    EDGE_WEEDS = ('CORNFLOWER_A', 'CORNFLOWER_B', 'CORNFLOWER_A', 'COCKLE', 'MAYWEED')
    BORDER_WEEDS = ('THISTLE', 'THISTLE', 'MAYWEED', 'MAYWEED', 'CORNFLOWER_B')

    for strip, kind in enumerate(order):
        lo, hi = edge(strip), edge(strip + 1)
        span = (bounds[strip] - 115, bounds[strip + 1] + 115)

        def standing(u, w):
            return kind == 'rye' or (kind == 'reaping' and behind(u, w) < 0)

        # the strip's own ground
        ridged = kind == 'plough'
        for piece, origin in life.strip_ground(ground_mesh.cards, ground, spot, u0, u1, lo, hi, ridged, rng):
            visual = ground_mesh(piece, EARTH if kind in ('plough', 'rye') else LITTER)
            record(visual, origin, np.eye(3))

        if kind in ('rye', 'reaping'):
            for w in np.arange(*span, ROW_GAP):
                for u in np.arange(u0 + rng.uniform(0, ROW_STEP), u1, ROW_STEP):
                    uu, ww = u + rng.normal(0, 12), w + rng.normal(0, 5)
                    inside = min(ww - lo(uu), hi(uu) - ww) - 20
                    if kind == 'reaping':
                        inside = min(inside, -behind(uu, ww))
                    if inside < EDGE:
                        continue                            # the edge stands as single tufts
                    p = spot(uu, ww)
                    if clear(p) and not patches.is_bare(p):
                        rye('ROWE' if inside < EDGE + 2.2 * ROW_GAP else 'ROW', 'ABC', p, tangent(uu))
            for u in np.arange(u0, u1, CROSS_GAP):
                for w in np.arange(span[0], span[1], ROW_STEP):
                    uu, ww = u + rng.normal(0, 10), w + rng.normal(0, 15)
                    inside = min(ww - lo(uu), hi(uu) - ww) - 20
                    if kind == 'reaping':
                        inside = min(inside, -behind(uu, ww))
                    p = spot(uu, ww)
                    if inside > EDGE + 60 and clear(p) and not patches.is_bare(p):
                        rye('ROW', 'ABC', p, across(uu), jitter=18.)
            # the reaped edge: single tufts, thinner outwards, some lower, a few left standing in the stubble;
            # a weed among them here and there
            tufts = round((u1 - u0) / 100 * (EDGE + 60) / 100 * TUFTS_M2)
            for side, sign in ((0, 1.), (1, -1.)):
                for _ in range(tufts):
                    uu = rng.uniform(u0, u1)
                    inward = rng.uniform(-60, EDGE)
                    if inward < 0 and rng.random() < .75:      # few of them outside the line
                        continue
                    if rng.random() > .35 + .65 * np.clip(inward / EDGE, 0, 1):
                        continue
                    ww = (lo(uu) if side == 0 else hi(uu)) + sign * (20 + inward)
                    if kind == 'reaping' and behind(uu, ww) > -EDGE * .5:
                        continue
                    p = spot(uu, ww)
                    if not clear(p):
                        continue
                    if rng.random() < .07:
                        weed(p, EDGE_WEEDS)
                    else:
                        rye('TUFT', 'ABCD', p, unit(rng.normal(0, 1, 2)), jitter=180.,
                            sink=(2., 14. + (rng.uniform(10, 45) if rng.random() < .2 else 0.)), low_share=.6)
            if kind == 'reaping':
                # the front: the sickle bites in handful by handful, so it is ragged and thin
                for _ in range(round((bounds[strip + 1] - bounds[strip]) / 100 * (EDGE + 60) / 100 * TUFTS_M2 * 1.3)):
                    ww = rng.uniform(bounds[strip], bounds[strip + 1])
                    inward = rng.uniform(-50, EDGE)
                    if rng.random() > .3 + .7 * np.clip(inward / EDGE, 0, 1):
                        continue
                    # behind() is monotonic in u: step back from the front into the standing rye
                    uu = front_u + 110 * math.sin(ww / 160 + ragged[0]) + 50 * math.sin(ww / 53 + ragged[1]) \
                        - front_way * inward
                    if not lo(uu) + 20 < ww < hi(uu) - 20:
                        continue
                    p = spot(uu, ww)
                    if clear(p):
                        rye('TUFT', 'ABCD', p, unit(rng.normal(0, 1, 2)), jitter=180., sink=(2., 14.), low_share=.6)
        if kind in ('stubble', 'reaping'):
            for w in np.arange(*span, STUBBLE_GAP):
                for u in np.arange(u0 + rng.uniform(0, ROW_STEP), u1, ROW_STEP):
                    if rng.random() < .2:                       # a trodden gap
                        continue
                    uu, ww = u + rng.normal(0, 30), w + rng.normal(0, 14)
                    if not lo(uu) + 5 < ww < hi(uu) - 5 or standing(uu, ww):
                        continue
                    add(f'{RYE}_STUBBLE_{"AB"[rng.integers(2)]}', *spot(uu, ww), tangent(uu), jitter=28.,
                        sink=rng.uniform(1, 6))
            for u in np.arange(u0 + 250, u1 - 150, STOOK_STEP):
                uu = u + rng.normal(0, 120)
                ww = (lo(uu) + hi(uu)) / 2 + rng.normal(0, (bounds[strip + 1] - bounds[strip]) * .14)
                if kind == 'reaping' and behind(uu, ww) < SHEAVES:
                    continue
                p = spot(uu, ww)
                if clear(p, 200):
                    add(f'{DRY}_STOOK_{"ABC"[rng.integers(3)]}', *p, unit(rng.normal(0, 1, 2)), jitter=180.,
                        lean=(unit(rng.normal(0, 1, 2)), math.radians(rng.uniform(0, 7))), collision='mesh')
                    if rng.random() < .3:                       # a sheaf that fell out of it, or was not stood yet
                        q = p + unit(rng.normal(0, 1, 2)) * rng.uniform(95, 140)
                        add(f'{DRY}_SHEAF_A', *q, unit(rng.normal(0, 1, 2)), jitter=180., sink=0.)
            if rng.random() < .5:                               # a rake left against... the ground by a stook
                uu = rng.uniform(u0 + 300, u1 - 300)
                p = spot(uu, (lo(uu) + hi(uu)) / 2 + rng.normal(0, 80))
                if not standing(uu, (lo(uu) + hi(uu)) / 2):
                    add(RAKE, *p, unit(rng.normal(0, 1, 2)), jitter=180., sink=-4.)
        if kind == 'reaping':
            width = bounds[strip + 1] - bounds[strip]

            def at_front(distance, ww):
                uu = front_u + 110 * math.sin(ww / 160 + ragged[0]) + 50 * math.sin(ww / 53 + ragged[1]) + front_way * distance
                return uu, ww

            # loose swaths: handfuls laid in lines across the strip, the stalks pointing away from the rye
            for distance in np.arange(70, SWATHS, 135):
                for ww in np.arange(bounds[strip] + 70, bounds[strip + 1] - 70, 150):
                    if rng.random() < .15:
                        continue
                    uu, ww2 = at_front(distance + rng.normal(0, 22), ww + rng.normal(0, 25))
                    if lo(uu) + 40 < ww2 < hi(uu) - 40:
                        add(f'{DRY}_SWATH_{"AB"[rng.integers(2)]}', *spot(uu, ww2), across(uu), jitter=14., sink=0.)
            # bound sheaves lying where they were tied
            for _ in range(round((SHEAVES - SWATHS) * width / 1e4 / 2.6)):
                uu, ww = at_front(rng.uniform(SWATHS, SHEAVES), rng.uniform(bounds[strip] + 60, bounds[strip + 1] - 60))
                if lo(uu) + 40 < ww < hi(uu) - 40:
                    add(f'{DRY}_SHEAF_{"AAB"[rng.integers(3)]}', *spot(uu, ww), across(uu), jitter=35., sink=0.)
            # the cart that takes them, a few sheaves in it, a sickle by the swaths
            uu, ww = at_front(SHEAVES * .75, (bounds[strip] + bounds[strip + 1]) / 2)
            p = spot(uu, ww)
            hit = ground.at(*p)
            if hit is not None and clear(p, 250):
                t = tangent(uu)
                side = np.array((-t[1], 0., t[0]))              # the cart's X across the strip, its length along it
                out = np.cross(side, (0., 1., 0.))
                frame = np.stack([side, np.array((0., 1., 0.)), out], axis=1)
                record(CART, (p[0], hit[0] + 49., p[1]), frame, 'mesh')
                for k, (dx, dy, dz) in enumerate(((-28., 62., -20.), (26., 64., 10.), (0., 88., -5.))):
                    q = np.array((p[0], hit[0] + 49. + dy, p[1])) + side * dx + out * dz
                    lay = np.stack([out, np.array((0., 1., 0.)), -side], axis=1)     # the sheaf's X along the cart
                    record(f'{DRY}_SHEAF_A', q - (0., 15., 0.), lay)
            uu, ww = at_front(SWATHS * .5, bounds[strip] + width * .3)
            add(SICKLE, *spot(uu, ww), unit(rng.normal(0, 1, 2)), jitter=180., sink=-3.)

    # weeds: in the bare spots of the rye, and thistle and mayweed along the field's border
    for spot, radius in patches.bare:
        for _ in range(round(math.pi * radius ** 2 / 1e4 * 2.5)):
            p = spot + unit(rng.normal(0, 1, 2)) * radius * math.sqrt(rng.random())
            if ground.at(*p) is not None and order[strip_at(p)] == 'rye' and clear(p):
                weed(p, EDGE_WEEDS + ('MAYWEED', 'THISTLE'))
    for _ in range(round(area * .9)):
        p = rng.uniform(low, high)
        if ground.at(*p) is None or not clear(p):
            continue
        if any(ground.at(p[0] + dx, p[1] + dz) is None for dx, dz in ((170, 0), (-170, 0), (0, 170), (0, -170))):
            weed(p, BORDER_WEEDS)
        elif order[strip_at(p)] == 'stubble' and rng.random() < .05:
            weed(p, ('MAYWEED', 'MAYWEED', 'CORNFLOWER_B'))

    own = {v: n for v, n in counts.items() if v in meshes}
    stats = dict(field_m2=round(area, 1), strips=list(order),
                 strip_widths_cm=[round(float(b - a), 1) for a, b in zip(bounds, bounds[1:])],
                 bend_cm=round(float(swing), 1), long_axis=[round(float(c), 4) for c in u_axis],
                 length_cm=round(float(u1 - u0), 1), lodged_spots=len(patches.lodged), bare_spots=len(patches.bare),
                 work_spots_and_items=len(keep), of_them_in_open=in_open(order), removed_plants=len(remove),
                 added=len(added), by_visual={v: n for v, n in sorted(own.items())},
                 originals={v: n for v, n in counts.items() if v in (CART, RAKE, SICKLE)},
                 ground_pieces=sum(n for v, n in counts.items() if v.startswith('OWN_CROP_GROUND')),
                 triangles=sum(meshes[v]['triangles'] * n for v, n in own.items()))
    return added, remove, stats


def atlas(listing: dict, passes, names: list, side: int):
    """The cards `names` packed into a square of `side` px: colour, alpha, the height share of a texel in its card
    (1 at the top), the kind of card a texel belongs to, and the cells by name."""
    margin = round(listing['margin_m'] * DENSITY)
    chosen = [c for c in listing['cards'] if c['name'] in names]
    places = pack({c['name']: c['px'] for c in chosen}, side)
    colour, alpha, rise = np.zeros((side, side, 3)), np.zeros((side, side)), np.ones((side, side))
    kinds = np.full((side, side), '', dtype=object)
    cells = {}
    for c in chosen:
        name, (width, height), (x, y) = c['name'], c['px'], places[c['name']]
        picture = passes[name].astype(np.float64)
        colour[y:y + height, x:x + width] = picture[..., :3]
        alpha[y:y + height, x:x + width] = picture[..., 3]
        rise[y:y + height, x:x + width] = np.linspace(1, 0, height)[:, None]
        kinds[y:y + height, x:x + width] = c['kind']
        cells[name] = dict(u=(x / side, (x + width) / side), v_root=(y + height - margin) / side, v_tip=y / side,
                           width_cm=c['width_m'] * 100, height_cm=(c['height_m'] - listing['margin_m']) * 100)
    return colour, alpha, rise, kinds, cells


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
    parser.add_argument('--preview', type=Path, help='write the atlases and the ground maps as PNG here and stop: no set')
    args = parser.parse_args()
    cards, dds, pilot = sibling('glb_cards.py'), sibling('test-material.py'), sibling('megascans_pilot.py')
    life = sibling('crop_field_life.py')

    output = STAGING / args.set
    if output.exists() and not args.preview:
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    if args.rerender or not (args.cards / 'cards.npz').exists():
        render(args.cards, args.seed)
    listing = json.loads((args.cards / 'cards.json').read_text())
    passes = np.load(args.cards / 'cards.npz')
    crop = [c['name'] for c in listing['cards'] if c['kind'] not in WEED_KINDS]
    wild = [c['name'] for c in listing['cards'] if c['kind'] in WEED_KINDS]
    colour, alpha, rise, kinds, cells = atlas(listing, passes, crop, ATLAS)
    for kind, tint in life.KIND_TINT.items():
        colour[kinds == kind] *= tint
    colour = np.clip(colour * args.gain, 0, 1)
    toned = {tone: np.clip(life.TONES[tone](colour, rise), 0, 1) for tone in TONES}
    weed_colour, weed_alpha, weed_rise, _, weed_cells = atlas(listing, passes, wild, WEED_ATLAS)
    weight = alpha[..., None]
    mean = {tone or 'RIPE': ((c * weight).sum(axis=(0, 1)) / weight.sum()).round(4).tolist() for tone, c in toned.items()}
    if args.preview:
        from PIL import Image
        args.preview.mkdir(parents=True, exist_ok=True)
        for tone, c in toned.items():
            Image.fromarray(np.dstack([pilot.encode(cards.spread_colour(c, alpha)),
                                       np.rint(alpha * 255).astype(np.uint8)])).save(args.preview / f'atlas{tone and "-" + tone.lower()}.png')
        Image.fromarray(np.dstack([pilot.encode(cards.spread_colour(weed_colour, weed_alpha)),
                                   np.rint(weed_alpha * 255).astype(np.uint8)])).save(args.preview / 'weeds.png')
        records = life.ground_maps(dds, pilot.encode, args.source / 'materials/meadow_earth', args.preview / 'crop_ground',
                                   np.random.default_rng(args.seed))
        for name in ('earth_albedo', 'litter_albedo', 'litter_normal'):
            Image.open(args.preview / 'crop_ground' / f'{name}.dds').convert('RGB').save(args.preview / f'{name}.png')
        print(json.dumps(dict(mean=mean, ground=records['means']), indent=2))
        return 0

    work = output.with_name(output.name + '.tmp')
    if work.exists():
        shutil.rmtree(work)
    subprocess.run(['cp', '-Rc', str(args.source), str(work)], check=True)     # an APFS clone: no bytes copied
    rng = np.random.default_rng(args.seed)
    meshes = build_meshes(cards, cells, weed_cells, work / 'meshes' / FOLDER, rng)

    small = ATLAS // 512

    def write_cards(folder, tones, alpha, rise):
        """The colour maps of an atlas (one per tone) and its ORM; returns the coverage per mip."""
        directory = work / 'materials' / folder
        directory.mkdir(parents=True)
        for tone, picture in tones.items():
            coverage = cards.write_cutout(dds, pilot.encode, directory / f'albedo{tone and "_" + tone.lower()}.dds',
                                          picture, alpha)
        side = alpha.shape[0] // small
        ao = cards.spread_colour((AO_ROOT + (1 - AO_ROOT) * rise)[..., None], alpha)[..., 0]
        ao = ao.reshape(side, small, side, small).mean(axis=(1, 3))
        dds.write_dds(directory / 'orm.dds', np.stack([ao, np.full_like(ao, ROUGH), np.zeros_like(ao)], axis=2).astype(np.float32))
        return coverage

    coverage = write_cards(FOLDER, toned, alpha, rise)
    write_cards('crop_weed', {'': weed_colour}, weed_alpha, weed_rise)
    ground = life.ground_maps(dds, pilot.encode, args.source / 'materials/meadow_earth', work / 'materials/crop_ground', rng)
    table = work / 'materials/materials.json'
    materials = json.loads(table.read_text())
    for tone in TONES:
        materials[f'{tone_material(tone)}.TGA'] = dict(albedo=f'{FOLDER}/albedo{tone and "_" + tone.lower()}.dds',
                                                       orm=f'{FOLDER}/orm.dds', two_sided='same', foliage=FOLIAGE)
    materials[f'{DRY}.TGA'] = dict(albedo=f'{FOLDER}/albedo.dds', orm=f'{FOLDER}/orm.dds', two_sided='same')
    materials[f'{WEED}.TGA'] = dict(albedo='crop_weed/albedo.dds', orm='crop_weed/orm.dds', two_sided='same',
                                    foliage=FOLIAGE | dict(wind=1.8))
    materials[f'{EARTH}.TGA'], materials[f'{LITTER}.TGA'] = ground['earth'], ground['litter']
    table.write_text(json.dumps(materials, indent=2) + '\n')

    def ground_mesh(mesh, material):
        name = f'OWN_CROP_GROUND_{sum(1 for m in meshes if m.startswith("OWN_CROP_GROUND")) + 1:03d}'
        mesh.write(work / 'meshes' / FOLDER / f'{name}.glb', material, group='EARTH')
        meshes[name] = dict(glb=f'{FOLDER}/{name}.glb', triangles=mesh.triangles, material=material)
        return name

    ground_mesh.cards = cards
    base = args.base or sibling('world/world_census.py').resolve_base(None)
    added, removed, stats = [], [], []
    for number, field in enumerate(args.field.split(';')):
        more, gone, numbers = place(base, args.world, tuple(map(float, field.split(','))), meshes, args.seed, number,
                                    life, ground_mesh)
        added, removed, stats = added + more, removed + gone, stats + [numbers]
    index_path = work / 'meshes/meshes.json'
    index = json.loads(index_path.read_text()) if index_path.exists() else {}
    index_path.write_text(json.dumps(index | {name: dict(glb=m['glb']) for name, m in meshes.items()}, indent=2) + '\n')
    layer = work / 'worlds' / args.world / 'vobs.d/crop-field.json'
    layer.parent.mkdir(parents=True, exist_ok=True)
    layer.write_text(json.dumps(dict(format=1, add=added, remove=removed), separators=(',', ':')) + '\n')
    report = dict(source=str(args.source), field=args.field, seed=args.seed, atlas_px=ATLAS, density_px_m=DENSITY,
                  gain=args.gain, mean=mean, coverage_by_mip=[round(c, 4) for c in coverage],
                  normal_up=NORMAL_UP, normal_edge=NORMAL_EDGE, foliage=FOLIAGE, ground=ground['means'],
                  meshes={name: m for name, m in meshes.items() if not name.startswith('OWN_CROP_GROUND')},
                  ground_triangles=sum(m['triangles'] for name, m in meshes.items() if name.startswith('OWN_CROP_GROUND')),
                  placement=stats)
    (work / 'crop-field.json').write_text(json.dumps(report, indent=2) + '\n')
    work.rename(output)
    print(json.dumps(report | dict(meshes=len(meshes)), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
