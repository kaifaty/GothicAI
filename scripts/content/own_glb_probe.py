#!/usr/bin/env python3
"""A staging set that answers by one filming how the engine draws a card of an own GLB with an alpha mask.

    uv run --no-project --with numpy --with scipy --with pillow python scripts/content/own_glb_probe.py glb-probe-2
    TEMPEST_WINDOW_SIZE=2560x1440 OPENGOTHIC_CONTENT=~/.local/share/gothic-ai/content-staging/glb-probe-2 \
        scripts/bench.sh own-glb-probe <out>

Panels of 40 cm hang in the air over the meadow of the `ledge` view (scripts/bench/own-glb-probe.bench shoots them
from the front and from behind). All of them stand in one plane and face the first camera; the colour map is
mid grey with round holes on its left half and an alpha ramp to nothing on its right.

Upper row, one side each, the same geometry with other vertex normals: toward the camera (the face's own), up,
down, away from the camera, and up at the top edge with down at the bottom one. If the normals reach the light
as written, the panels differ as surfaces turned that way would.
Lower row, between two flags: a panel wound toward the first camera, one wound away from it, and one added twice,
wound both ways with the same normals. A one-sided material shows two of the three from either side.
The flags (a pole along the mesh's +Y, a pennant toward its +X at the top) check the layer's `rotation`: the left
one is not turned, the right one has the rows (0,0,1), (1,0,0), (0,1,0), which lay the pole along the world's +Z,
away from the first camera, and stand the pennant up.

The sides of a material (`"two_sided"` of its record, reports/engine/2026-11-16-modern-trees/two-sided.md) are a
second group over the meadow east of the tavern: a row of cards along Z over a grey plate that takes their
shadows, every card tilted to the noon sun (it stands at -X, 57 degrees high), shot from the sun's side, from
the lee one and from the lee side low toward the sun. Seven cards, SIDES, twice: wound to the sun at -Z and wound away from it at +Z, the normals written
for the side of the winding.

    one          one copy, a one-sided material: one side is drawn
    pair         two copies wound both ways with the same normals (glb_cards' pairs, the trees' cards before)
    same         one copy, "two_sided": "same" — has to look and to shade as `pair`
    proper       two copies, the second with the normals of its own side
    flip         one copy, "two_sided": "flip" — has to look and to shade as `proper`
    proper-leaf  `proper` of a leaf's material: "foliage" with no wind and translucency 0.6
    flip-leaf    `flip` of the same leaf's material — has to let the sun through as `proper-leaf`
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
CENTRE = (35110., 4100., 429.)   # 2.7 m over the hero's spot of the `ledge` view
MATERIAL = 'OWN_GLB_PROBE'
SIDES_CENTRE = (40550., 3610., 400.)   # 2 m over the meadow, 7.5 m north of the hero's second spot
SIDES = ('one', 'pair', 'same', 'proper', 'flip', 'proper-leaf', 'flip-leaf')
SIDES_STEP, SIDES_PLATE = 50., -70.
LEAF = dict(anchor='line', wind=0., flutter=0., translucency=.6)
# material: the fields of its record besides the colour map, and the cards it draws
SIDES_MATERIALS = {MATERIAL: ({}, ('one', 'pair', 'proper')),
                   f'{MATERIAL}_SAME': (dict(two_sided='same'), ('same',)),
                   f'{MATERIAL}_FLIP': (dict(two_sided='flip'), ('flip',)),
                   f'{MATERIAL}_LEAF': (dict(foliage=LEAF), ('proper-leaf',)),
                   f'{MATERIAL}_FLIP_LEAF': (dict(two_sided='flip', foliage=LEAF), ('flip-leaf',))}


def sibling(name: str):
    path = HERE / name
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def panel(mesh, x, y, normals, front, both=False, half=20.):
    """A square in the plane z = 0; normals: one for all corners, or (bottom, top)."""
    bottom, top = normals if isinstance(normals[0], tuple) else (normals, normals)
    corners = [((x - half, y - half, 0), bottom, (0, 1)), ((x + half, y - half, 0), bottom, (1, 1)),
               ((x + half, y + half, 0), top, (1, 0)), ((x - half, y + half, 0), top, (0, 0))]
    mesh.quad(corners, front, both)


def sides(cards) -> dict:
    """material -> the mesh of its cards of the second group. A card is a square of 40 cm whose plane faces
    (-0.8, 0.6, 0), 3 degrees off the noon sun."""
    sun, along, slope = np.array((-.8, .6, 0.)), np.array((0., 0., 1.)), np.array((.6, .8, 0.))
    meshes = {}
    for material, (_, kinds) in SIDES_MATERIALS.items():
        mesh = meshes[material] = cards.Mesh()
        for kind in kinds:
            for wound in (1, -1):       # to the sun at -Z, away from it at +Z
                centre = np.array((0., 0., -wound * SIDES_STEP * (1 + SIDES.index(kind))))
                front = sun * wound

                def quad(side, normal, both=False):
                    mesh.quad([(tuple(centre + 20 * (a * along + b * slope)), tuple(normal), ((a + 1) / 2, (1 - b) / 2))
                               for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))], tuple(side), both)

                quad(front, front, both=kind in ('same', 'flip', 'flip-leaf'))
                if kind == 'pair':
                    quad(-front, front)
                if kind in ('proper', 'proper-leaf'):
                    quad(-front, -front)
    return meshes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    args = parser.parse_args()
    output = STAGING / args.set
    if output.exists():
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    cards, dds, pilot = sibling('glb_cards.py'), sibling('test-material.py'), sibling('megascans_pilot.py')

    toward, away, up, down = (0, 0, -1), (0, 0, 1), (0, 1, 0), (0, -1, 0)
    panels = cards.Mesh(pairs=True)
    for x, normals in zip((-100, -50, 0, 50, 100), (toward, up, down, away, (down, up))):
        panel(panels, x, 30, normals, toward)
    panel(panels, -50, -30, toward, toward)
    panel(panels, 0, -30, toward, away)
    panel(panels, 50, -30, toward, toward, both=True)
    flag = cards.Mesh(pairs=True)
    for corners in ([(-2, 0), (2, 0), (2, 40), (-2, 40)], [(2, 30), (22, 30), (22, 40), (2, 40)]):
        flag.quad([((x, y, 0), toward, (.1, .5)) for x, y in corners], toward, both=True)

    (output / 'meshes/glb-probe').mkdir(parents=True)
    panels.write(output / 'meshes/glb-probe/panels.glb', MATERIAL)
    flag.write(output / 'meshes/glb-probe/flag.glb', MATERIAL)
    meshes = {'OWN_GLB_PROBE_PANELS': dict(glb='glb-probe/panels.glb'),
              'OWN_GLB_PROBE_FLAG': dict(glb='glb-probe/flag.glb')}
    records = {f'{MATERIAL}.TGA': dict(albedo='glb-probe/albedo.dds')}
    side_cards = sides(cards)
    for number, (material, mesh) in enumerate(side_cards.items()):
        assert mesh.two_sided == ('two_sided' in SIDES_MATERIALS[material][0])
        mesh.write(output / f'meshes/glb-probe/sides-{number}.glb', material)
        meshes[f'OWN_GLB_PROBE_SIDES_{number}'] = dict(glb=f'glb-probe/sides-{number}.glb')
        records[f'{material}.TGA'] = dict(albedo='glb-probe/albedo.dds', **SIDES_MATERIALS[material][0])
    plate, reach = cards.Mesh(), SIDES_STEP * (len(SIDES) + 1.4)
    plate.quad([((x, SIDES_PLATE, z), up, (.5, .5)) for x, z in ((-60, -reach), (130, -reach), (130, reach), (-60, reach))],
               up)
    plate.write(output / 'meshes/glb-probe/plate.glb', f'{MATERIAL}_PLATE')
    meshes['OWN_GLB_PROBE_PLATE'] = dict(glb='glb-probe/plate.glb')
    records[f'{MATERIAL}_PLATE.TGA'] = dict(albedo='glb-probe/plate.dds')
    (output / 'meshes/meshes.json').write_text(json.dumps(meshes, indent=2) + '\n')

    size = 256
    v, u = np.mgrid[0:size, 0:size] / size
    holes = np.hypot((u * 8) % 1 - .5, (v * 8) % 1 - .5) > .3
    alpha = np.where(u < .5, holes.astype(np.float64), np.clip((1 - u) / .5, 0, 1))
    colour = np.full((size, size, 3), .2)
    (output / 'materials/glb-probe').mkdir(parents=True)
    cards.write_cutout(dds, pilot.encode, output / 'materials/glb-probe/albedo.dds', colour, alpha)
    cards.write_cutout(dds, pilot.encode, output / 'materials/glb-probe/plate.dds', np.full((16, 16, 3), .35),
                       np.ones((16, 16)))
    (output / 'materials/materials.json').write_text(json.dumps(records, indent=2) + '\n')

    x, y, z = CENTRE
    add = [dict(key='glb-probe-panels', kind='mesh', visual='OWN_GLB_PROBE_PANELS', position=[x, y, z], yaw_deg=0,
                collision='none'),
           dict(key='glb-probe-flag-plain', kind='mesh', visual='OWN_GLB_PROBE_FLAG', position=[x - 110, y - 50, z],
                yaw_deg=0, collision='none'),
           dict(key='glb-probe-flag-turned', kind='mesh', visual='OWN_GLB_PROBE_FLAG', position=[x + 110, y - 50, z],
                rotation=[[0, 0, 1], [1, 0, 0], [0, 1, 0]], collision='none')]
    add += [dict(key=f'glb-probe-{visual[len("OWN_GLB_PROBE_"):].lower()}', kind='mesh', visual=visual,
                 position=list(SIDES_CENTRE), yaw_deg=0, collision='none')
            for visual in meshes if visual.startswith(('OWN_GLB_PROBE_SIDES', 'OWN_GLB_PROBE_PLATE'))]
    (output / 'worlds/NEWWORLD/vobs.d').mkdir(parents=True)
    (output / 'worlds/NEWWORLD/vobs.d/glb-probe.json').write_text(json.dumps(dict(format=1, add=add), indent=1) + '\n')
    print(f'{output}: {panels.triangles} + {flag.triangles} triangles, '
          f'the sides {[mesh.triangles for mesh in side_cards.values()]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
