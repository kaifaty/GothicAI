#!/usr/bin/env python3
"""Build a staging set with a rough spruce that shows the tree's wind: a probe of the motion, not of a tree's look.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/tree_wind_probe.py tree-wind-probe-1

Task 1 of reports/engine/2026-11-16-modern-trees/engine-plan.md; what the probe is for and what it showed:
reports/engine/2026-11-16-modern-trees/tree-wind.md. No Blender: the mesh is written straight to a GLB.

- the spruce, `meshes/tree_wind_probe/`: HEIGHT cm from the root at the mesh's origin up +Y. A conical trunk, three
  tiers of branches (TIERS), a spray of needles along every branch (a flat card either side of the branch and a
  card hanging under it) and two crossed cards at the top. Two primitives, the bark and the needles. Every card is
  added with both sides, as glb_cards.py does: the engine draws one side of a triangle. VARIANTS meshes differ by
  the seed of the branches' turns;
- COLOR_0 by the table of the plan (graphics/foliagetree.h): R the weight of the branch, 0 at the trunk and 1 at
  the branch's end, a card's vertex takes the weight of the branch's point it sits at; G one phase a branch; B the
  weight of the needle's flutter, 0 on the branch and 1 at the card's free edge; A the openness of the crown, 1 on
  the trunk and outside, less inside;
- the materials, `materials/tree_wind_probe/`: a drawn spray of needles with the cut-out in alpha
  (glb_cards.write_cutout) and a drawn bark, a flat ORM each. Both records say `"foliage": {"anchor": "tree"}` with
  the same `wind`: the trunk leans by the height of the whole mesh, so the bark and the needles go together;
- the places, `worlds/NEWWORLD/vobs.d/tree-wind-probe.json`: SPOTS on the pilot's meadow east of the tavern, each
  on the ground found under it in the base. The cameras of scripts/bench/tree-wind-probe.bench stand by these
  spots: change both together.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import struct
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
FOLDER = 'tree_wind_probe'
VISUAL = 'OWN_TREE_WIND_PROBE_SPRUCE'
BARK, NEEDLES = 'OWN_TREE_WIND_PROBE_BARK', 'OWN_TREE_WIND_PROBE_NEEDLES'
HEIGHT = 1200.              # cm
TRUNK_SIDES = 8
TRUNK_FOOT = 17.            # the trunk's radius at the root, cm; 1.5 at the top
SUNK = 60.                  # the trunk goes this far under the root: the ground under a tree is not level
# height of the tier over the root, branches in it, a branch's length (cm)
TIERS = ((300., 7, 340.), (590., 6, 260.), (860., 5, 175.))
STATIONS = 6                # points along a branch
VARIANTS = (('A', 20261116), ('B', 20261117))
FOLIAGE_BARK = dict(anchor='tree', wind=1., flutter=0., translucency=0.)
FOLIAGE_NEEDLES = dict(anchor='tree', wind=1., flutter=1., translucency=.35)
# x, z of the root, the turn about Y (degrees), the variant. The first is the one the cameras look at
SPOTS = ((41300., 0., 0., 'A'), (41050., -420., 70., 'B'), (41550., 380., 140., 'B'), (41600., -300., 200., 'A'),
         (41000., 430., 260., 'A'), (40650., 480., 320., 'B'))
RECT = (4, -2, 7, 1)        # the base's chunks the ground under the spots is looked for in
UP = np.array((0., 1., 0.))


def sibling(name: str):
    path = HERE / name
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def unit(v):
    v = np.asarray(v, np.float64)
    return v / np.linalg.norm(v)


def part(cards):
    """glb_cards.Mesh whose corners carry a colour as well: (position, normal, uv, colour 0..1)."""
    class Part(cards.Mesh):
        def __init__(self):
            super().__init__(pairs=True)    # the probe of the wind stays as it was filmed: its records have one side
            self.colour = []

        def triangle(self, corners, front) -> None:
            a, b, c = (np.asarray(corner[0], np.float64) for corner in corners)
            if np.dot(np.cross(b - a, c - a), front) > 0:    # X is mirrored on the way to glTF
                corners = (corners[0], corners[2], corners[1])
            for position, normal, uv, colour in corners:
                self.index.append(len(self.position))
                self.position.append(tuple(float(v) for v in position))
                self.normal.append(tuple(float(v) for v in normal))
                self.uv.append(tuple(float(v) for v in uv))
                self.colour.append(tuple(int(round(255 * min(max(float(v), 0.), 1.))) for v in colour))
    return Part()


def tube(mesh, centres, radii, colours, sides: int, v_scale: float) -> None:
    """A closed tube round the points `centres`; one colour a ring."""
    rings = []
    for index, (centre, radius) in enumerate(zip(centres, radii)):
        along = unit(centres[min(index + 1, len(centres) - 1)] - centres[max(index - 1, 0)])
        first = unit(np.cross(along, (0., 0., 1.) if abs(along[2]) < .9 else (1., 0., 0.)))
        second = np.cross(along, first)
        ring = []
        for side in range(sides + 1):
            angle = 2 * math.pi * side / sides
            out = first * math.cos(angle) + second * math.sin(angle)
            ring.append((centre + out * radius, out, (2. * side / sides, centre[1] / v_scale), colours[index]))
        rings.append(ring)
    for low, high in zip(rings, rings[1:]):
        for side in range(sides):
            mesh.quad([low[side], low[side + 1], high[side + 1], high[side]], low[side][1] + low[side + 1][1])


def spruce(cards, seed: int):
    rng = np.random.default_rng(seed)
    bark, needles = part(cards), part(cards)

    heights = np.array((-SUNK, 0., 150., 400., 700., 1000., HEIGHT))
    radii = np.where(heights > 0, (TRUNK_FOOT - 1.5) * (1 - np.clip(heights, 0, HEIGHT) / HEIGHT) ** .9 + 1.5,
                     TRUNK_FOOT * 1.25)
    radii[1] = TRUNK_FOOT * 1.1
    trunk = (0., 0., 0., 1.)    # no branch, no flutter, open
    tube(bark, [np.array((0., y, 0.)) for y in heights], radii, [trunk] * len(heights), TRUNK_SIDES, 200.)

    branches = 0
    for tier, (height, count, length) in enumerate(TIERS):
        start = rng.uniform(0, 2 * math.pi)
        for number in range(count):
            turn = start + 2 * math.pi * (number + rng.uniform(-.18, .18)) / count
            out = np.array((math.cos(turn), 0., math.sin(turn)))
            across = np.array((-out[2], 0., out[0]))
            reach = length * rng.uniform(.88, 1.08)
            root = height + rng.uniform(-35., 35.)
            phase = rng.uniform(0., 1.)
            share = np.linspace(0., 1., STATIONS)
            # a spruce's branch: droops from the trunk and turns up at its end
            axis = [out * (reach * s) + UP * (root - .26 * reach * s + .13 * reach * s * s) for s in share]
            open_ = [.3 + .7 * s for s in share]
            tube(bark, axis, 6. - 4.5 * share, [(s, phase, 0., a) for s, a in zip(share, open_)], 4, 200.)

            lit = unit(.75 * UP + .5 * out)      # the cards are lit as a crown, not each by its own plane
            wide = [.3 * reach * math.sin(math.pi * min(.12 + .88 * s, 1.) ** .75) ** .8 + 10. for s in share]
            hang = [.17 * reach * math.sin(math.pi * (.08 + .84 * s)) ** .7 + 8. for s in share]
            flat, curtain = [], []
            for point, s, a, half, drop in zip(axis, share, open_, wide, hang):
                middle = (point, lit, (.5, s), (s, phase, 0., a))
                # the weight of the branch is the same across the card: it goes as the branch's point it sits at
                flat.append((middle,
                             (point + across * half - UP * (.3 * half), lit, (1., s), (s, phase, 1., 1.)),
                             (point - across * half - UP * (.3 * half), lit, (0., s), (s, phase, 1., 1.))))
                curtain.append((middle, (point - UP * drop, unit(.4 * UP + .8 * out), (1., s), (s, phase, 1., a))))
            for low, high in zip(flat, flat[1:]):
                for edge in (1, 2):
                    needles.quad([low[0], low[edge], high[edge], high[0]], UP, both=True)
            for low, high in zip(curtain, curtain[1:]):
                needles.quad([low[0], low[1], high[1], high[0]], across, both=True)
            branches += 1

    # the top: two crossed cards on the leader, their middle on the trunk
    top_phase = rng.uniform(0., 1.)
    for turn in (rng.uniform(0, math.pi), None):
        turn = turn if turn is not None else last + math.pi / 2
        last = turn
        out = np.array((math.cos(turn), 0., math.sin(turn)))
        rows = []
        for y, half, v in ((960., 95., 0.), (1050., 70., .35), (1150., 38., .7), (HEIGHT + 35., 6., 1.)):
            point = np.array((0., y, 0.))
            rows.append(((point, UP, (.5, v), (0., top_phase, 0., 1.)),
                         (point + out * half - UP * (.35 * half), unit(UP + out), (1., v), (0., top_phase, 1., 1.)),
                         (point - out * half - UP * (.35 * half), unit(UP - out), (0., v), (0., top_phase, 1., 1.))))
        for low, high in zip(rows, rows[1:]):
            for edge in (1, 2):
                needles.quad([low[0], low[edge], high[edge], high[0]], np.cross(out, UP), both=True)
    return bark, needles, branches


def write_glb(path: Path, parts) -> None:
    """parts: (Part, material name, group, alpha mode). Gothic centimetres to glTF's metres, X mirrored."""
    chunks, views, accessors, primitives, materials, offset = [], [], [], [], [], 0

    def add(array, target, **accessor):
        nonlocal offset
        data = array.tobytes()
        data += b'\0' * (-len(data) % 4)
        views.append(dict(buffer=0, byteOffset=offset, byteLength=array.nbytes, target=target))
        accessors.append(dict(bufferView=len(views) - 1, count=len(array), **accessor))
        chunks.append(data)
        offset += len(data)
        return len(accessors) - 1

    for mesh, material, group, mode in parts:
        if len(mesh.position) > 65535:
            raise SystemExit(f'{material}: {len(mesh.position)} vertices; an own mesh takes 65535 a material')
        position = np.asarray(mesh.position, np.float32) * np.array((-.01, .01, .01), np.float32)
        normal = np.asarray(mesh.normal, np.float32) * np.array((-1, 1, 1), np.float32)
        normal /= np.linalg.norm(normal, axis=1, keepdims=True)
        attributes = dict(
            POSITION=add(position, 34962, componentType=5126, type='VEC3',
                         min=position.min(axis=0).tolist(), max=position.max(axis=0).tolist()),
            NORMAL=add(normal, 34962, componentType=5126, type='VEC3'),
            TEXCOORD_0=add(np.asarray(mesh.uv, np.float32), 34962, componentType=5126, type='VEC2'),
            COLOR_0=add(np.asarray(mesh.colour, np.uint8), 34962, componentType=5121, normalized=True, type='VEC4'))
        indices = add(np.asarray(mesh.index, np.uint16), 34963, componentType=5123, type='SCALAR')
        primitives.append(dict(attributes=attributes, indices=indices, material=len(materials), mode=4))
        record = dict(name=material, doubleSided=False, extras=dict(group=group))
        if mode == 'MASK':
            record.update(alphaMode='MASK', alphaCutoff=.5)
        materials.append(record)
    document = dict(asset=dict(version='2.0', generator='tree_wind_probe.py'), scene=0, scenes=[dict(nodes=[0])],
                    nodes=[dict(mesh=0, name=path.stem)], meshes=[dict(name=path.stem, primitives=primitives)],
                    materials=materials, accessors=accessors, bufferViews=views, buffers=[dict(byteLength=offset)])
    text = json.dumps(document, separators=(',', ':')).encode()
    text += b' ' * (-len(text) % 4)
    blob = b''.join(chunks)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'glTF' + struct.pack('<II', 2, 28 + len(text) + len(blob))
                     + struct.pack('<I', len(text)) + b'JSON' + text
                     + struct.pack('<I', len(blob)) + b'BIN\0' + blob)


def spray(size: int, seed: int):
    """A twig with side twigs and needles as wide as the picture, the root at v = 0 and the twig along u = 0.5:
    linear colour, coverage."""
    from PIL import Image, ImageDraw
    rng = np.random.default_rng(seed)
    over = 4
    big = size * over
    green, wood = Image.new('L', (big, big), 0), Image.new('L', (big, big), 0)
    pen_green, pen_wood = ImageDraw.Draw(green), ImageDraw.Draw(wood)

    def twig(a, b, width: float, needle: float) -> None:
        a, b = np.asarray(a, np.float64) * big, np.asarray(b, np.float64) * big
        pen_wood.line([tuple(a), tuple(b)], fill=255, width=max(round(width * big), 1))
        along = b - a
        run = np.linalg.norm(along)
        along /= run
        side = np.array((-along[1], along[0]))
        step = .0075 * big
        for place in np.arange(0., run, step):
            for sign in (-1, 1):
                root = a + along * (place + rng.uniform(-.4, .4) * step)
                lean = rng.uniform(.45, .85)
                tip = root + (along * lean + side * sign * math.sqrt(1 - lean * lean)) * needle * big * rng.uniform(.7, 1.1)
                pen_green.line([tuple(root), tuple(tip)], fill=255, width=max(round(.0055 * big), 1))

    twig((.5, 0.), (.5, .97), .009, .05)
    for index, v in enumerate(np.arange(-.24, .93, .042)):
        sign = 1 if index % 2 else -1
        reach = .5 * rng.uniform(.82, 1.)      # the card's outline narrows the spray, not the picture
        end = (.5 + sign * .82 * reach, min(v + .57 * reach, .98))
        twig((.5, v), end, .005, .042)
        for share in (.35, .65):       # twigs of the second order, back towards the middle
            base = np.array((.5, v)) + (np.array(end) - (.5, v)) * share
            twig(base, base + np.array((-sign * .1, .9)) * .16 * reach / .5, .004, .036)

    def small(image):
        return np.asarray(image, np.float64).reshape(size, over, size, over).mean(axis=(1, 3)) / 255

    needles, twigs = small(green), small(wood)
    alpha = np.clip(needles + twigs, 0, 1)
    v = np.linspace(0, 1, size)[:, None]
    grain = np.random.default_rng(seed + 1).uniform(.75, 1.25, (size, size, 1))
    young = np.array((.034, .066, .028)) * (.8 + .5 * v)[..., None] * grain    # paler to the tip: this year's growth
    woody = np.array((.07, .045, .028))
    share = (twigs / np.maximum(alpha, 1e-6))[..., None]
    return young * (1 - share) + woody * share, alpha


def bark_picture(size: int, seed: int) -> np.ndarray:
    """Scaly brown bark, linear colour: streaks along the trunk."""
    from scipy.ndimage import gaussian_filter
    rng = np.random.default_rng(seed)
    streak = gaussian_filter(rng.standard_normal((size, size)), (size / 18, size / 90), mode='wrap')
    scale = gaussian_filter(rng.standard_normal((size, size)), size / 120, mode='wrap')
    value = np.clip(.5 + .28 * streak / streak.std() + .12 * scale / scale.std(), 0, 1)
    return (np.array((.045, .03, .02)) + np.array((.11, .075, .05)) * value[..., None]).astype(np.float64)


def ground_under(base: Path, world: str, cards_spots, places):
    """The highest ground of the base under each (x, z): its height and the texture it is drawn with."""
    triangles, names = [], []
    for cx in range(RECT[0], RECT[2] + 1):
        for cz in range(RECT[1], RECT[3] + 1):
            path = base / 'worlds' / world / 'chunks' / f'{cx}_{cz}.glb'
            if not path.exists():
                continue
            for name, _, position, _, _ in cards_spots.chunk(path):
                triangles.append(position)
                names += [name] * len(position)
    if not triangles:
        raise SystemExit(f'{base}: no chunks of {world} in {RECT}')
    t = np.concatenate(triangles)
    a, b, c = t[:, 0], t[:, 1], t[:, 2]
    area = (b[:, 2] - c[:, 2]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 2] - c[:, 2])
    flat = np.abs(area) > 1e-9
    area = np.where(flat, area, 1.)
    found = []
    for x, z in places:
        w0 = ((b[:, 2] - c[:, 2]) * (x - c[:, 0]) + (c[:, 0] - b[:, 0]) * (z - c[:, 2])) / area
        w1 = ((c[:, 2] - a[:, 2]) * (x - c[:, 0]) + (a[:, 0] - c[:, 0]) * (z - c[:, 2])) / area
        hit = np.nonzero(flat & (w0 >= 0) & (w1 >= 0) & (w0 + w1 <= 1))[0]
        if not len(hit):
            raise SystemExit(f'no ground of {world} under ({x}, {z})')
        y = w0[hit] * a[hit, 1] + w1[hit] * b[hit, 1] + (1 - w0[hit] - w1[hit]) * c[hit, 1]
        top = int(np.argmax(y))
        found.append((float(y[top]), names[hit[top]]))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    parser.add_argument('--world', default='NEWWORLD')
    parser.add_argument('--base', type=Path, help='the imported base (default: the one in ~/.cache/gothic-ai/base)')
    parser.add_argument('--preview', type=Path, help='write the two pictures as PNG here and stop: no set')
    args = parser.parse_args()
    cards, dds, pilot = sibling('glb_cards.py'), sibling('test-material.py'), sibling('megascans_pilot.py')
    colour, alpha = spray(512, 20261116)
    bark = bark_picture(256, 20261116)
    if args.preview:
        from PIL import Image
        args.preview.mkdir(parents=True, exist_ok=True)
        shown = np.dstack([pilot.encode(cards.spread_colour(colour, alpha)), np.rint(alpha * 255).astype(np.uint8)])
        Image.fromarray(shown).save(args.preview / 'needles.png')
        Image.fromarray(pilot.encode(bark)).save(args.preview / 'bark.png')
        print(f'coverage {alpha.mean():.3f}')
        return 0

    output = STAGING / args.set
    if output.exists():
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    base = args.base or next(Path.home().glob('.cache/gothic-ai/base/*/worlds')).parent
    ground = ground_under(base, args.world, sibling('world/landscape_tuft_spots.py'), [(x, z) for x, z, _, _ in SPOTS])

    work = output.with_name(output.name + '.tmp')
    if work.exists():
        import shutil
        shutil.rmtree(work)
    meshes, report = {}, dict(height_cm=HEIGHT, tiers=TIERS, meshes={}, spots=[])
    for letter, seed in VARIANTS:
        trunk, needles, branches = spruce(cards, seed)
        visual = f'{VISUAL}_{letter}'
        write_glb(work / 'meshes' / FOLDER / f'{visual}.glb', ((trunk, BARK, 'WOOD', 'OPAQUE'), (needles, NEEDLES, 'UNDEF', 'MASK')))
        meshes[visual] = dict(glb=f'{FOLDER}/{visual}.glb')
        points = np.asarray(trunk.position + needles.position)
        report['meshes'][visual] = dict(seed=seed, branches=branches, triangles=dict(bark=trunk.triangles, needles=needles.triangles),
                                        box=[points.min(axis=0).round(1).tolist(), points.max(axis=0).round(1).tolist()])
    (work / 'meshes/meshes.json').write_text(json.dumps(meshes, indent=2) + '\n')

    directory = work / 'materials' / FOLDER
    directory.mkdir(parents=True)
    coverage = cards.write_cutout(dds, pilot.encode, directory / 'needles.dds', colour, alpha)
    dds.write_dds(directory / 'bark.dds', (pilot.encode(bark) / 255).astype(np.float32))
    for name, rough, ao in (('needles_orm', .85, .8), ('bark_orm', .95, .9)):
        dds.write_dds(directory / f'{name}.dds', np.full((16, 16, 3), (ao, rough, 0.), np.float32))
    materials = {f'{BARK}.TGA': dict(albedo=f'{FOLDER}/bark.dds', orm=f'{FOLDER}/bark_orm.dds', foliage=FOLIAGE_BARK),
                 f'{NEEDLES}.TGA': dict(albedo=f'{FOLDER}/needles.dds', orm=f'{FOLDER}/needles_orm.dds',
                                        foliage=FOLIAGE_NEEDLES)}
    (work / 'materials/materials.json').write_text(json.dumps(materials, indent=2) + '\n')

    added = []
    for number, ((x, z, turn, letter), (y, texture)) in enumerate(zip(SPOTS, ground), 1):
        added.append(dict(key=f'tree-wind-probe-{number}', kind='mesh', visual=f'{VISUAL}_{letter}',
                          position=[x, round(y, 1), z], yaw_deg=turn, collision='none'))
        report['spots'].append(dict(position=[x, round(y, 1), z], yaw_deg=turn, visual=f'{VISUAL}_{letter}', ground=texture))
    layer = work / 'worlds' / args.world / 'vobs.d/tree-wind-probe.json'
    layer.parent.mkdir(parents=True)
    layer.write_text(json.dumps(dict(format=1, add=added), indent=1) + '\n')

    report.update(materials=materials, needles_coverage_by_mip=[round(c, 4) for c in coverage])
    (work / 'tree-wind-probe.json').write_text(json.dumps(report, indent=2) + '\n')
    work.rename(output)
    print(json.dumps(report, indent=2))
    print(output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
