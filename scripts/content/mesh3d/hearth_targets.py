#!/usr/bin/env python3
"""Measure scalar targets for the kitchen and hearth set (asset city-hearth, wave 1: open fire).

    python3 scripts/content/mesh3d/hearth_targets.py [--base DIR] [--output data/content/mesh3d-hearth.json]
                                                     [--underlay DIR]

Same record shape as rigging_targets.py: bbox in glTF metres (-x, y, z)/100, origin as a fraction of the
bbox, source triangles. Static visuals are read from the converted base's meshes/<VISUAL>.glb, mob models
from models/<VISUAL>.MDL|MDM|MDH.json in their bind pose (model space, as the loader expects the GLB of a
mob). Only names and numbers leave the base.

--underlay DIR also writes each original as an OBJ in the same glTF metres, for underlay_check.py and
for Claude's own comparison in Blender. Those files hold the original's vertices: they stay outside the
repository and never go to a generator or into a model.
"""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
BASE_CACHE = Path.home() / '.cache/gothic-ai/base'


def zone(key: str, group: str, role: str) -> dict:
    return {'key': key, 'group': group, 'role': role}


IRON = zone('OWN_HEARTH_IRON', 'METAL', 'forged and riveted iron: cauldrons, pan, chain, fire bowl')
RIVET = zone('OWN_HEARTH_RIVET', 'METAL', 'bands and rivet heads on the iron')
LIQUID = zone('OWN_HEARTH_LIQUID', 'UNDEF', 'the surface of what is cooking (not WATER: the engine keeps that group for the world mesh)')
STONE = zone('OWN_HEARTH_STONE', 'STONE', 'fire-ring fieldstones, sooted inside')
EMBER = zone('OWN_HEARTH_EMBER', 'UNDEF', 'glowing embers')
ASH = zone('OWN_HEARTH_ASH', 'UNDEF', 'grey ash and burnt ground')
WOOD = zone('OWN_HEARTH_WOOD', 'WOOD', 'dark hewn sticks, poles and handles')
LOG = zone('OWN_HEARTH_LOG', 'WOOD', 'split logs with bark, charred ends')
CAST = zone('OWN_HEARTH_CAST', 'METAL', 'cast-iron stove plates, rusty brown')
COPPER = zone('OWN_HEARTH_COPPER', 'METAL', 'the hammered copper hood over the stove')
MASONRY = zone('OWN_HEARTH_MASONRY', 'STONE', 'rubble and block masonry of a fireplace')
ASHLAR = zone('OWN_HEARTH_ASHLAR', 'STONE', 'pale dressed stone of a rich fireplace')
CARVING = zone('OWN_HEARTH_CARVING', 'STONE', 'dressed stone: the mantle shelf, the hearth slab, mouldings and the hood relief')
SOOT = zone('OWN_HEARTH_SOOT', 'STONE', 'the sooted firebox: back wall, hearth floor')

TARGETS = [
    {'id': 'hearth-cauldron', 'visual': 'CAULDRON_OC', 'budget_tris': 2500, 'zones': [IRON, RIVET, LIQUID]},
    {'id': 'hearth-ground-fire', 'visual': 'FIREPLACE_GROUND', 'budget_tris': 2000, 'zones': [STONE, EMBER, ASH, LOG]},
    {'id': 'hearth-pan', 'visual': 'PAN_OC', 'budget_tris': 2500, 'zones': [IRON, WOOD, STONE, EMBER, ASH]},
    {'id': 'hearth-tripod', 'visual': 'OC_FIREPLACE_V5', 'budget_tris': 3000, 'zones': [WOOD, IRON, RIVET, LIQUID]},
    {'id': 'hearth-campfire', 'visual': 'NW_MISC_FIREPLACE_01', 'budget_tris': 4000, 'zones': [STONE, LOG, EMBER, ASH]},
    {'id': 'hearth-brazier', 'visual': 'DT_FIREPLACE_V1', 'budget_tris': 3000, 'zones': [IRON, RIVET, EMBER, ASH]},
    # wave 2: hearths in the houses
    {'id': 'hearth-stove', 'visual': 'STOVE_NW_CITY_01', 'budget_tris': 3000, 'zones': [CAST, RIVET, COPPER, EMBER, WOOD]},
    {'id': 'hearth-fireplace', 'visual': 'NW_CITY_FIREPLACE_NORMAL_01', 'budget_tris': 3000, 'zones': [MASONRY, CARVING, SOOT, LOG, EMBER]},
    {'id': 'hearth-fireplace-large', 'visual': 'NW_CITY_FIREPLACE_LARGE_01', 'budget_tris': 4000, 'zones': [MASONRY, CARVING, SOOT, LOG, EMBER]},
    {'id': 'hearth-fireplace-wide', 'visual': 'NW_CITY_FIREPLACE_LARGE_02', 'budget_tris': 4000, 'zones': [ASHLAR, CARVING, SOOT, LOG, EMBER]},
    {'id': 'hearth-fireplace-wide-low', 'visual': 'NW_CITY_FIREPLACE_LARGE_021', 'budget_tris': 4000, 'zones': [ASHLAR, CARVING, SOOT, LOG, EMBER]},
]


def glb_parts(path: Path):
    """[(positions in glTF metres, triangles, texture)] of a base static mesh (stored in Gothic centimetres)."""
    data = path.read_bytes()
    length = struct.unpack('<I', data[12:16])[0]
    gltf = json.loads(data[20:20 + length])
    blob = data[20 + length + 8:]

    def accessor(index):
        a = gltf['accessors'][index]
        view = gltf['bufferViews'][a['bufferView']]
        dtype = {5126: '<f4', 5123: '<u2', 5125: '<u4'}[a['componentType']]
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[a['type']]
        offset = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        return np.frombuffer(blob, dtype, a['count'] * width, offset).reshape(a['count'], width)

    parts = []
    for mesh in gltf['meshes']:
        for prim in mesh['primitives']:
            texture = gltf['materials'][prim['material']]['extras']['gothic']['texture']
            parts.append((accessor(prim['attributes']['POSITION']).astype(float) * np.array([-1, 1, 1]) / 100,
                          accessor(prim['indices']).reshape(-1, 3), texture))
    return parts


def model_parts(models: Path, visual: str):
    """[(positions in glTF metres, triangles, texture)] of a mob model in its bind pose."""
    source = next(models / f'{visual}{ext}.json' for ext in ('.MDL', '.MDM') if (models / f'{visual}{ext}.json').exists())
    record = json.loads(source.read_text())
    blob = (models / record['buffer']).read_bytes()
    hierarchy = record['data'].get('hierarchy') or json.loads((models / f'{visual}.MDH.json').read_text())['data']
    world = []
    root = np.array(hierarchy.get('root_translation') or [0, 0, 0], dtype=float)
    for node in hierarchy['nodes']:
        local = np.array(node['transform'], dtype=float).reshape(4, 4).T
        if node['parent_index'] < 0:  # the engine's global matrices include the root translation (PAN_OC: 0.8 m)
            local = local.copy()
            local[:3, 3] += root
        world.append(local if node['parent_index'] < 0 else world[node['parent_index']] @ local)
    names = [node['name'] for node in hierarchy['nodes']]
    mesh = record['data'].get('mesh') or record['data']
    wedge = np.dtype([('n', '<f4', 3), ('uv', '<f4', 2), ('i', '<u2')])
    parts = []
    for attachment in mesh['attachments']:
        matrix = world[names.index(attachment['name'])]
        m = attachment['mesh']
        positions = np.frombuffer(blob, '<f4', m['positions']['count'] * 3, m['positions']['offset']).reshape(-1, 3)
        for sub in m['sub_meshes']:
            t, w = sub['triangles'], sub['wedges']
            tris = np.frombuffer(blob, '<u2', t['count'] * 3, t['offset']).reshape(-1, 3)
            wedges = np.frombuffer(blob[w['offset']:w['offset'] + wedge.itemsize * w['count']], wedge)
            p = positions[wedges['i']].astype(float)
            p = (matrix @ np.c_[p, np.ones(len(p))].T).T[:, :3]
            parts.append((p * np.array([-1, 1, 1]) / 100, tris, sub['mat']['texture']))
    nodes = {name: (world[i][:3, 3] * np.array([-1, 1, 1]) / 100).round(4).tolist()
             for i, name in enumerate(names) if not any(a['name'] == name for a in mesh['attachments'])}
    return parts, nodes


def measure(base: Path, target: dict, underlay: Path | None) -> dict:
    visual = target['visual']
    glb = base / 'meshes' / f'{visual}.glb'
    nodes = None
    if glb.exists():
        parts, kind = glb_parts(glb), 'static_mrm'
    else:
        parts, nodes = model_parts(base / 'models', visual)
        kind = 'mob_model'
    points = np.vstack([p for p, _, _ in parts])
    lo, hi = points.min(axis=0), points.max(axis=0)
    record = {**target, 'kind': kind, 'bbox_min': lo.round(5).tolist(), 'bbox_max': hi.round(5).tolist(),
              'origin_in_bbox': (-lo / np.where(hi - lo > 0, hi - lo, 1)).round(6).tolist(),
              'source_tris': int(sum(len(t) for _, t, _ in parts)),
              'source_materials': sorted({tex for _, _, tex in parts})}
    if kind == 'mob_model':
        record['source_pose'] = 'bind'
        record['nodes'] = nodes
    if underlay:
        underlay.mkdir(parents=True, exist_ok=True)
        lines, offset = [], 0
        for p, tris, tex in parts:
            lines += [f'v {x:.5f} {y:.5f} {z:.5f}' for x, y, z in p]
            lines.append(f'g {tex}')
            lines += [f'f {a + 1 + offset} {b + 1 + offset} {c + 1 + offset}' for a, b, c in tris]
            offset += len(p)
        (underlay / f'{visual}.obj').write_text('\n'.join(lines) + '\n')
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--base', type=Path, default=BASE_CACHE / 'current')
    parser.add_argument('--output', type=Path, default=ROOT / 'data/content/mesh3d-hearth.json')
    parser.add_argument('--underlay', type=Path)
    args = parser.parse_args()
    base = args.base.resolve()
    if (base / 'meshes').exists() is False:  # the cache root: the newest imported base
        base = max((p for p in base.iterdir() if (p / 'meshes').exists()), key=lambda p: p.stat().st_mtime)
    targets = [measure(base, t, args.underlay) for t in TARGETS]
    args.output.write_text(json.dumps({'base': base.name, 'targets': targets}, indent=1, ensure_ascii=False) + '\n')
    print(args.output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
