#!/usr/bin/env python3
"""Build a staging set whose grass band on the rock has tufts of real leaves: meshes of cards over the layer.

    uv run --no-project --with numpy --with pillow --with scipy python \
        scripts/content/landscape_grass_tufts.py rock-grass-tufts-1 [--from rock-grass-3] [--rect 4,-1,5,0] \
        [--normal-up .75] [--preview DIR]

Step 2 of reports/engine/2026-11-13-grass/README.md. Copies a set with the baked grass layer
(landscape_grass_layer.py) and adds, without touching what it copies:

- one atlas of card pictures, `materials/landscape_grass_tufts/`: side views of clusters of the same scanned
  tufts the layer is baked from (mesh3d/blender/grass_cards.py; rendered once into the work folder, `--rerender`
  to render again). A picture is darkened from tip to root, FLOOR + (1 - FLOOR) * (height share) ** CURVE, the
  depth the engine's light does not give a card; its pale straw root takes the colour of the leaves; then the
  whole atlas is brought per channel to the mean colour of the layer it stands on (`grass-layer.json` of the
  copied set), so the same calibration carries over. Alpha is the texel's coverage and every mip averages it
  (glb_cards.write_cutout): the engine's hashed alpha then keeps the share of grass at any distance.
  ORM: roughness ROUGH (matte, as the layer's blades), a weak root-to-tip AO, no metal;
- the tufts, `meshes/landscape_grass_tufts/`: SHAPES in SIZES (the object layer has no scale). A hanging card is
  a strip of three quads that leaves the rock at the root, bends over and hangs along the tuft's -Y a few
  centimetres off the surface; an upright one is two quads along the axis between "up the surface" and "out of
  it". Every card is added with both sides (the engine draws one), and its vertex normals are `--normal-up` of
  the direction up and out of the rock and the rest the card's own, so cards turned differently are lit alike
  and close to the layer under them;
- the places, `worlds/<WORLD>/vobs.d/landscape-grass-tufts.json` (world/landscape_tuft_spots.py).
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import shutil
import subprocess
from pathlib import Path

import numpy as np
from scipy.ndimage import gaussian_filter1d

HERE = Path(__file__).parent
SOURCES = Path.home() / '.local/share/gothic-ai/content-sources/landscape-probe'
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
SCAN = 'grass_medium_01'    # Poly Haven, CC0: the tufts the layer is baked from
ATLAS = 2048
DENSITY = 1024              # pixels per metre of a card
MATERIAL = 'OWN_LANDSCAPE_GRASS_TUFTS'
VISUAL = 'OWN_LANDSCAPE_GRASS_TUFT'
CURVE = 1.5                 # brightness = height share ** 1.5 over FLOOR, as the layer's
FLOOR = .4
TIP = .75                   # a leaf is at full brightness from this share of the tallest beside it
ROOT_HUE = .85              # how much of the leaves' mean colour the root takes: the scan's is pale straw
ROUGH = .92
AO_ROOT = .7
NORMAL_UP = .75             # share of the common direction in a vertex normal
COMMON = (0., .45, .9)      # that direction in the tuft's axes: out of the rock and up the surface
SIZES = (1., .7)

# A tuft's cards. hang: x of the root, swing in the rock's plane, twist about the hanging axis (degrees), how far
# it stands off at the bend (cm). up: turn about the growth axis (degrees).
SHAPES = dict(
    LONG=(('hang', 'hang_long_a', dict(x=0, swing=2, twist=8, out=9)),
          ('hang', 'hang_long_b', dict(x=-9, swing=-16, twist=-38, out=7)),
          ('hang', 'hang_mid_a', dict(x=10, swing=17, twist=35, out=11))),
    MID=(('hang', 'hang_mid_a', dict(x=0, swing=-3, twist=-6, out=9)),
         ('hang', 'hang_mid_b', dict(x=-10, swing=-18, twist=36, out=7)),
         ('hang', 'droop_b', dict(x=8, swing=15, twist=-32, out=11))),
    WISP=(('hang', 'hang_long_b', dict(x=0, swing=6, twist=12, out=8)),
          ('hang', 'hang_mid_b', dict(x=4, swing=-14, twist=-30, out=10))),
    DROOP=(('hang', 'droop_a', dict(x=0, swing=-5, twist=10, out=12)),
           ('hang', 'droop_thin', dict(x=6, swing=20, twist=-35, out=9)),
           ('up', 'clump_b', dict(turn=20)),
           ('up', 'clump_thin', dict(turn=110))),
    CLUMP=(('up', 'clump_a', dict(turn=0)),
           ('up', 'clump_b', dict(turn=60)),
           ('up', 'clump_low', dict(turn=120))),
)


def sibling(name: str):
    path = HERE / name
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def luma(rgb: np.ndarray) -> np.ndarray:
    return rgb @ np.array((.2126, .7152, .0722))


def unit(v):
    v = np.asarray(v, np.float64)
    return v / np.linalg.norm(v)


def render(folder: Path, density: int, seed: int) -> None:
    blender = os.environ.get('BLENDER') or shutil.which('blender') or '/Applications/Blender.app/Contents/MacOS/Blender'
    subprocess.run([blender, '-b', '-P', str(HERE / 'mesh3d/blender/grass_cards.py'), '--',
                    '--source', str(SOURCES / SCAN / f'{SCAN}_4k.gltf'), '--out', str(folder),
                    '--density', str(density), '--seed', str(seed)], check=True)


def pack(sizes: dict, side: int) -> dict:
    """Shelves, tallest first: name -> (x, y) of the picture's corner."""
    x = y = shelf = 0
    places = {}
    for name, (width, height) in sorted(sizes.items(), key=lambda item: -item[1][1]):
        width, height = -(-width // 4) * 4, -(-height // 4) * 4   # whole DXT blocks
        if x + width > side:
            x, y, shelf = 0, y + shelf, 0
        places[name] = (x, y)
        x, shelf = x + width, max(shelf, height)
    if y + shelf > side:
        raise SystemExit(f'the cards do not fit {side} px')
    return places


def shade(colour: np.ndarray, alpha: np.ndarray, height: np.ndarray, texel_m: float):
    """The picture darkened to the root, the scan's pale root brought to the leaves' hue and brightness; the
    height share."""
    covered = alpha > .5
    top = np.where(covered, height, 0).max(axis=0)                   # the tallest leaf of a column ...
    top = np.maximum(gaussian_filter1d(top, .04 / texel_m), .5 * top.max())   # ... of its 4 cm, not under half the card's
    share = np.clip(height / (TIP * top[None, :]), 0, 1)
    leaves = covered & (share > .5)
    leaf = colour[leaves].mean(axis=0)
    weight = (ROOT_HUE * (1 - share) ** 2)[..., None]
    colour = colour * (1 - weight) + leaf * weight
    return colour * (FLOOR + (1 - FLOOR) * share ** CURVE)[..., None], share


def hang_card(mesh, cell: dict, normal_up: float, x: float, swing: float, twist: float, out: float) -> None:
    """A strip from the root over a bend and down the tuft's -Y; the picture's root is at the strip's start."""
    width, length = cell['width_cm'], cell['length_cm']
    path = np.array([(1.5, -3.), (4., out), (-.42 * length, .8 * out), (-(length - 13.), .4 * out)])  # (y, z)
    run = np.concatenate([[0.], np.cumsum(np.linalg.norm(np.diff(path, axis=0), axis=1))])
    twist, swing = math.radians(twist), math.radians(swing)
    across = np.array((math.cos(twist), 0., math.sin(twist)))
    turn = np.array(((math.cos(swing), -math.sin(swing), 0.), (math.sin(swing), math.cos(swing), 0.), (0., 0., 1.)))
    common = unit(COMMON)
    rows = []
    for index, ((y, z), wide) in enumerate(zip(path, (.7, .85, 1., 1.1))):
        ahead = path[min(index + 1, len(path) - 1)] - path[max(index - 1, 0)]
        along = unit((0., ahead[0], ahead[1]))
        own = unit(np.cross(along, across))
        own = own if own @ np.array((0., .5, 1.)) > 0 else -own
        normal = turn @ unit(normal_up * common + (1 - normal_up) * own)
        half = width * wide / 2
        lift = half * abs(math.sin(twist)) if index else 0.     # the edge nearer the rock stays off it
        centre = np.array((0., y, z + lift))
        v = cell['v_root'] + (cell['v_tip'] - cell['v_root']) * run[index] / run[-1]
        rows.append([(turn @ (centre + across * side * half) + (x, 0., 0.), normal, (u, v))
                     for side, u in ((-1, cell['u'][0]), (1, cell['u'][1]))])
    for low, high in zip(rows, rows[1:]):
        mesh.quad([low[0], low[1], high[1], high[0]], np.cross(low[1][0] - low[0][0], high[0][0] - low[0][0]), both=True)


def up_card(mesh, cell: dict, normal_up: float, turn: float) -> None:
    """Two quads along the axis between up the surface and out of it, bending up; crossed cards share the axis."""
    width, length = cell['width_cm'], cell['length_cm']
    axis = unit((0., 1., 1.))
    turn = math.radians(turn)
    flat = np.array((1., 0., 0.)), unit(np.cross(axis, (1., 0., 0.)))
    across = flat[0] * math.cos(turn) + flat[1] * math.sin(turn)
    common = unit(COMMON)
    stations = [(0., 0.), (.45, .03), (1., .16)]     # share of the length, bend toward +Y as a share of it
    centres = [axis * (share * length - 2.) + np.array((0., bend * length, 0.)) for share, bend in stations]
    rows = []
    for index, (share, _) in enumerate(stations):
        along = unit(centres[min(index + 1, 2)] - centres[max(index - 1, 0)])
        own = unit(np.cross(along, across))
        own = own if own @ np.array((0., .5, 1.)) > 0 else -own
        normal = unit(normal_up * common + (1 - normal_up) * own)
        v = cell['v_root'] + (cell['v_tip'] - cell['v_root']) * share
        rows.append([(centres[index] + across * side * width / 2, normal, (u, v))
                     for side, u in ((-1, cell['u'][0]), (1, cell['u'][1]))])
    for low, high in zip(rows, rows[1:]):
        mesh.quad([low[0], low[1], high[1], high[0]], np.cross(low[1][0] - low[0][0], high[0][0] - low[0][0]), both=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    parser.add_argument('--from', dest='source', default='rock-grass-3',
                        help='staging set with the grass layer to copy (default: %(default)s)')
    parser.add_argument('--cards', type=Path, default=SOURCES / 'work/grass-tufts', help='folder of the rendered cards')
    parser.add_argument('--rerender', action='store_true')
    parser.add_argument('--seed', type=int, default=11)
    parser.add_argument('--world', default='NEWWORLD')
    parser.add_argument('--base', type=Path)
    parser.add_argument('--rect', default='4,-1,5,0', help='X0,Z0,X1,Z1 in chunk coordinates, inclusive')
    parser.add_argument('--normal-up', type=float, default=NORMAL_UP,
                        help='share of the direction up and out of the rock in a vertex normal (default: %(default)s)')
    parser.add_argument('--gain', type=float, default=1., help='the atlas against the layer\'s mean colour')
    parser.add_argument('--preview', type=Path, help='write the atlas as PNG here and stop: no set')
    args = parser.parse_args()

    source, output = STAGING / args.source, STAGING / args.set
    if output.exists() and not args.preview:
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    layer = json.loads((source / 'grass-layer.json').read_text())
    cards, dds, pilot = sibling('glb_cards.py'), sibling('test-material.py'), sibling('megascans_pilot.py')
    spots = sibling('world/landscape_tuft_spots.py')

    if args.rerender or not (args.cards / 'cards.npz').exists():
        render(args.cards, DENSITY, args.seed)
    listing = json.loads((args.cards / 'cards.json').read_text())
    if (listing['density'], listing['seed']) != (DENSITY, args.seed):
        raise SystemExit(f'{args.cards}: rendered at {listing["density"]} px/m, seed {listing["seed"]}; --rerender')
    passes = np.load(args.cards / 'cards.npz')
    margin = round(listing['margin_m'] * DENSITY)
    places = pack({card['name']: card['px'] for card in listing['cards']}, ATLAS)

    colour = np.zeros((ATLAS, ATLAS, 3))
    alpha = np.zeros((ATLAS, ATLAS))
    share = np.zeros((ATLAS, ATLAS))
    cells = {}
    for card in listing['cards']:
        name, (width, height), (x, y) = card['name'], card['px'], places[card['name']]
        picture = passes[name].astype(np.float64)
        shaded, rise = shade(picture[..., :3], picture[..., 3], passes[f'{name}_height'].astype(np.float64), 1 / DENSITY)
        cover = picture[..., 3]
        if card['hang']:        # the root at the top
            shaded, cover, rise = shaded[::-1], cover[::-1], rise[::-1]
        colour[y:y + height, x:x + width] = shaded
        alpha[y:y + height, x:x + width] = cover
        share[y:y + height, x:x + width] = rise
        root, tip = (y + margin, y + height) if card['hang'] else (y + height - margin, y)
        cells[name] = dict(u=(x / ATLAS, (x + width) / ATLAS), v_root=root / ATLAS, v_tip=tip / ATLAS,
                           width_cm=card['width_m'] * 100, length_cm=(card['height_m'] - listing['margin_m']) * 100,
                           coverage=card['coverage'])
    weight = alpha[..., None]
    mean = (colour * weight).sum(axis=(0, 1)) / weight.sum()
    gains = np.array(layer['mean']) / mean * args.gain
    colour = np.clip(colour * gains, 0, 1)
    mean = (colour * weight).sum(axis=(0, 1)) / weight.sum()
    tips = alpha * (share > .8)
    report = dict(source=args.source, scan=SCAN, cards=listing, atlas_px=ATLAS, density_px_m=DENSITY,
                  curve=CURVE, floor=FLOOR, root_hue=ROOT_HUE, rough=ROUGH, ao_root=AO_ROOT,
                  normal_up=args.normal_up, common=list(COMMON), layer_mean=layer['mean'],
                  gains=gains.round(3).tolist(), mean=mean.round(4).tolist(),
                  tips_mean=((colour * tips[..., None]).sum(axis=(0, 1)) / tips.sum()).round(4).tolist(),
                  roots_mean=((colour * (alpha * (share < .15))[..., None]).sum(axis=(0, 1))
                              / (alpha * (share < .15)).sum()).round(4).tolist())
    if args.preview:
        from PIL import Image
        args.preview.mkdir(parents=True, exist_ok=True)
        ground = np.array(layer['mean'])
        shown = pilot.encode(colour * weight + ground * (1 - weight))
        Image.fromarray(shown).save(args.preview / 'atlas-on-layer.png')
        Image.fromarray(np.dstack([pilot.encode(cards.spread_colour(colour, alpha)),
                                   np.rint(alpha * 255).astype(np.uint8)])).save(args.preview / 'atlas.png')
        print(json.dumps({k: v for k, v in report.items() if k != 'cards'}, indent=2))
        return 0

    tufts = []
    meshes = {}
    folder = 'landscape_grass_tufts'
    work = output.with_name(output.name + '.tmp')
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(source, work)
    for shape, parts in SHAPES.items():
        for scale in SIZES:
            mesh = cards.Mesh()
            for kind, card, how in parts:
                if kind == 'hang':
                    hang_card(mesh, cells[card], args.normal_up, **how)
                else:
                    up_card(mesh, cells[card], args.normal_up, **how)
            mesh.position = [tuple(c * scale for c in p) for p in mesh.position]
            visual = f'{VISUAL}_{shape}_{round(scale * 100):03d}'
            mesh.write(work / 'meshes' / folder / f'{visual}.glb', MATERIAL)
            meshes[visual] = dict(glb=f'{folder}/{visual}.glb')
            hanging = max((cells[card]['length_cm'] - 13. for kind, card, _ in parts if kind == 'hang'), default=0.)
            tufts.append(dict(visual=visual, shape=shape, scale=scale, triangles=mesh.triangles,
                              cards=[card for _, card, _ in parts],
                              reach_cm=round(hanging * scale if shape != 'DROOP' else 0., 1),
                              hang_cm=round(hanging * scale, 1)))
    index_path = work / 'meshes/meshes.json'
    index = json.loads(index_path.read_text()) if index_path.exists() else {}
    index_path.write_text(json.dumps(index | meshes, indent=2) + '\n')

    directory = work / 'materials' / folder
    directory.mkdir(parents=True)
    coverage = cards.write_cutout(dds, pilot.encode, directory / 'albedo.dds', colour, alpha)
    small = ATLAS // 512
    ao = cards.spread_colour((AO_ROOT + (1 - AO_ROOT) * share)[..., None], alpha)[..., 0]
    ao = ao.reshape(512, small, 512, small).mean(axis=(1, 3))
    orm = np.stack([ao, np.full_like(ao, ROUGH), np.zeros_like(ao)], axis=2).astype(np.float32)
    dds.write_dds(directory / 'orm.dds', orm)
    materials = json.loads((work / 'materials/materials.json').read_text())
    materials[f'{MATERIAL}.TGA'] = dict(albedo=f'{folder}/albedo.dds', orm=f'{folder}/orm.dds')
    (work / 'materials/materials.json').write_text(json.dumps(materials, indent=2) + '\n')

    base = args.base or next(Path.home().glob('.cache/gothic-ai/base/*/worlds')).parent
    records = {name.removesuffix('.TGA'): record['fringe'] for name, record in materials.items() if 'fringe' in record}
    added, stats = spots.place(base / 'worlds' / args.world, tuple(map(int, args.rect.split(','))), records, tufts)
    layer_path = work / 'worlds' / args.world / 'vobs.d/landscape-grass-tufts.json'
    layer_path.parent.mkdir(parents=True, exist_ok=True)
    layer_path.write_text(json.dumps(dict(format=1, add=added), separators=(',', ':')) + '\n')

    def size(path: Path) -> int:
        return sum(f.stat().st_size for f in path.rglob('*') if f.is_file())

    report.update(coverage_by_mip=[round(c, 4) for c in coverage], tufts=tufts, rect=args.rect, placement=stats,
                  bytes=dict(atlas=(directory / 'albedo.dds').stat().st_size, orm=(directory / 'orm.dds').stat().st_size,
                             meshes=size(work / 'meshes' / folder), layer=layer_path.stat().st_size))
    (work / 'grass-tufts.json').write_text(json.dumps(report, indent=2) + '\n')
    work.rename(output)
    report['bytes']['set'] = size(output)
    print(json.dumps({k: v for k, v in report.items() if k != 'cards'}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
