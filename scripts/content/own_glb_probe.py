#!/usr/bin/env python3
"""A staging set that answers by one filming how the engine draws a card of an own GLB with an alpha mask.

    uv run --no-project --with numpy --with scipy python scripts/content/own_glb_probe.py glb-probe-1
    TEMPEST_WINDOW_SIZE=2560x1440 OPENGOTHIC_CONTENT=~/.local/share/gothic-ai/content-staging/glb-probe-1 \
        scripts/bench.sh own-glb-probe <out>

Panels of 40 cm hang in the air over the meadow of the `ledge` view (scripts/bench/own-glb-probe.bench shoots them
from the front and from behind). All of them stand in one plane and face the first camera; the colour map is
mid grey with round holes on its left half and an alpha ramp to nothing on its right.

Upper row, one side each, the same geometry with other vertex normals: toward the camera (the face's own), up,
down, away from the camera, and up at the top edge with down at the bottom one. If the normals reach the light
as written, the panels differ as surfaces turned that way would.
Lower row, between two flags: a panel wound toward the first camera, one wound away from it, and one added twice,
wound both ways with the same normals. A one-sided engine shows two of the three from either side.
The flags (a pole along the mesh's +Y, a pennant toward its +X at the top) check the layer's `rotation`: the left
one is not turned, the right one has the rows (0,0,1), (1,0,0), (0,1,0), which lay the pole along the world's +Z,
away from the first camera, and stand the pennant up.
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('set')
    args = parser.parse_args()
    output = STAGING / args.set
    if output.exists():
        raise SystemExit(f'{output}: staging set exists; choose a new set name')
    cards, dds, pilot = sibling('glb_cards.py'), sibling('test-material.py'), sibling('megascans_pilot.py')

    toward, away, up, down = (0, 0, -1), (0, 0, 1), (0, 1, 0), (0, -1, 0)
    panels = cards.Mesh()
    for x, normals in zip((-100, -50, 0, 50, 100), (toward, up, down, away, (down, up))):
        panel(panels, x, 30, normals, toward)
    panel(panels, -50, -30, toward, toward)
    panel(panels, 0, -30, toward, away)
    panel(panels, 50, -30, toward, toward, both=True)
    flag = cards.Mesh()
    for corners in ([(-2, 0), (2, 0), (2, 40), (-2, 40)], [(2, 30), (22, 30), (22, 40), (2, 40)]):
        flag.quad([((x, y, 0), toward, (.1, .5)) for x, y in corners], toward, both=True)

    (output / 'meshes/glb-probe').mkdir(parents=True)
    panels.write(output / 'meshes/glb-probe/panels.glb', MATERIAL)
    flag.write(output / 'meshes/glb-probe/flag.glb', MATERIAL)
    (output / 'meshes/meshes.json').write_text(json.dumps({
        'OWN_GLB_PROBE_PANELS': dict(glb='glb-probe/panels.glb'),
        'OWN_GLB_PROBE_FLAG': dict(glb='glb-probe/flag.glb')}, indent=2) + '\n')

    size = 256
    v, u = np.mgrid[0:size, 0:size] / size
    holes = np.hypot((u * 8) % 1 - .5, (v * 8) % 1 - .5) > .3
    alpha = np.where(u < .5, holes.astype(np.float64), np.clip((1 - u) / .5, 0, 1))
    colour = np.full((size, size, 3), .2)
    (output / 'materials/glb-probe').mkdir(parents=True)
    cards.write_cutout(dds, pilot.encode, output / 'materials/glb-probe/albedo.dds', colour, alpha)
    (output / 'materials/materials.json').write_text(json.dumps({
        f'{MATERIAL}.TGA': dict(albedo='glb-probe/albedo.dds')}, indent=2) + '\n')

    x, y, z = CENTRE
    add = [dict(key='glb-probe-panels', kind='mesh', visual='OWN_GLB_PROBE_PANELS', position=[x, y, z], yaw_deg=0,
                collision='none'),
           dict(key='glb-probe-flag-plain', kind='mesh', visual='OWN_GLB_PROBE_FLAG', position=[x - 110, y - 50, z],
                yaw_deg=0, collision='none'),
           dict(key='glb-probe-flag-turned', kind='mesh', visual='OWN_GLB_PROBE_FLAG', position=[x + 110, y - 50, z],
                rotation=[[0, 0, 1], [1, 0, 0], [0, 1, 0]], collision='none')]
    (output / 'worlds/NEWWORLD/vobs.d').mkdir(parents=True)
    (output / 'worlds/NEWWORLD/vobs.d/glb-probe.json').write_text(json.dumps(dict(format=1, add=add), indent=1) + '\n')
    print(f'{output}: {panels.triangles} + {flag.triangles} triangles')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
