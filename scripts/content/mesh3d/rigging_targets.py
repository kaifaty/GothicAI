#!/usr/bin/env python3
"""Measure scalar targets for the harbour rigging set (asset harbour-rigging, step M3).

    python3 scripts/content/mesh3d/rigging_targets.py [--base DIR] [--output data/content/mesh3d-rigging.json]

Same record shape as measure_targets.py (bbox in glTF metres (-x, y, z)/100, origin as a fraction
of the bbox, source triangles), read from the converted base's own meshes (meshes/<VISUAL>.glb,
docs/asset-migration.md) instead of the game archives. Only names and numbers leave the base.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[3]
BASE_CACHE = Path.home() / '.cache/gothic-ai/base'


def zone(key: str, group: str, role: str) -> dict:
    return {'key': key, 'group': group, 'role': role}


PLANK = zone('OWN_PROP_WOOD_PLANK', 'WOOD', 'box plank faces')
BRACE = zone('OWN_PROP_WOOD_BRACE', 'WOOD', 'corner posts and battens')
NAIL = zone('OWN_PROP_IRON_NAIL', 'METAL', 'small forged nail heads')
ROPE = zone('OWN_PROP_ROPE', 'UNDEF', 'twisted hemp rope')
INNER = zone('OWN_PROP_WOOD_INNER', 'WOOD', 'dark faces seen only through gaps')
CAST = zone('OWN_PROP_IRON_CAST', 'METAL', 'cast iron: cannon barrel, bollard')
BORE = zone('OWN_PROP_IRON_BORE', 'METAL', 'the dark bore inside the muzzle')
FRAME = zone('OWN_PROP_WOOD_FRAME', 'WOOD', 'hewn beams, carriage cheeks, wheels')
IRON = zone('OWN_PROP_IRON_BAND', 'METAL', 'forged straps, caps, pins and bolts')
NET = zone('OWN_PROP_NET', 'UNDEF', 'knotted hemp net, alpha-tested meshes')
NET_ROLL = zone('OWN_PROP_NET_ROLL', 'UNDEF', 'rolled-up net bundles, opaque')

TARGETS = [
    {'id': 'rig-crate-broken', 'visual': 'NW_HARBOUR_CRATEBROKEN_01', 'budget_tris': 2500,
     'zones': [PLANK, BRACE, NAIL, ROPE]},
    {'id': 'rig-rope', 'visual': 'NW_HARBOUR_ROPE_01', 'budget_tris': 4200, 'zones': [ROPE]},
    {'id': 'rig-cannon', 'visual': 'NW_HARBOUR_CANON_01', 'budget_tris': 3500, 'zones': [CAST, BORE, FRAME, IRON]},
    {'id': 'rig-net-big', 'visual': 'NW_HARBOUR_FISHINGNETBIG', 'budget_tris': 3600, 'zones': [NET, ROPE, NET_ROLL]},
    {'id': 'rig-net-small', 'visual': 'NW_HARBOUR_FISHINGNETSMALL', 'budget_tris': 2400, 'zones': [NET, ROPE, NET_ROLL]},
    {'id': 'rig-net-rack', 'visual': 'NW_HARBOUR_FISHINGNET_01', 'budget_tris': 9000,
     'zones': [FRAME, NET, NET_ROLL, ROPE, IRON]},
    {'id': 'rig-bollard-rope', 'visual': 'NW_HARBOUR_IRONROPEHOLDER_WROPE_01', 'budget_tris': 2000, 'zones': [CAST, ROPE]},
    {'id': 'rig-bollard', 'visual': 'NW_HARBOUR_IRONROPEHOLDER_01', 'budget_tris': 1200, 'zones': [CAST]},
]


def newest_base() -> Path:
    pointers = sorted((BASE_CACHE / 'current').glob('*.json'), key=lambda p: p.stat().st_mtime, reverse=True)
    for pointer in pointers:
        path = Path(json.loads(pointer.read_text()).get('base', {}).get('path', ''))
        if path.is_dir():
            return path
    raise SystemExit('no base: pass --base')


def measure(glb: Path) -> dict:
    data = glb.read_bytes()
    length = struct.unpack_from('<I', data, 12)[0]
    doc = json.loads(data[20:20 + length])
    gothic = doc['meshes'][0]['extras']['gothic']
    tris = sum(doc['accessors'][p['indices']]['count'] // 3 for p in doc['meshes'][0]['primitives'])
    lo, hi = gothic['bbox']
    # Gothic cm -> glTF metres: X mirrored.
    bmin = [-hi[0] / 100, lo[1] / 100, lo[2] / 100]
    bmax = [-lo[0] / 100, hi[1] / 100, hi[2] / 100]
    return {'bbox_min': [round(v, 6) for v in bmin], 'bbox_max': [round(v, 6) for v in bmax],
            'origin_in_bbox': [round(-a / (b - a), 6) for a, b in zip(bmin, bmax)],
            'source_tris': tris, 'source_materials': [s['texture'] for s in gothic['submeshes']]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--base', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'data/content/mesh3d-rigging.json')
    args = parser.parse_args()
    base = args.base or newest_base()
    targets = [{**t, 'kind': 'static_mrm', **measure(base / 'meshes' / f"{t['visual']}.glb")} for t in TARGETS]
    args.output.write_text(json.dumps({'source': 'converted base meshes', 'targets': targets}, indent=1) + '\n')
    for t in targets:
        size = [round(b - a, 2) for a, b in zip(t['bbox_min'], t['bbox_max'])]
        print(t['id'], t['visual'], size, t['origin_in_bbox'], t['source_tris'])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
