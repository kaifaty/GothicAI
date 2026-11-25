"""Recreate B1's asset-wide wind padding and a box for the whole crown."""

import hashlib
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4]))
from scripts.content.world.chunk_glb import accessor, read_glb


GLTF_TO_ENGINE = np.array([-100, 100, 100], dtype=np.float32)
BOUNDS_TOLERANCE_CM = np.float32(0.01)
MAX_DEPTH = np.float32(64)


def packed_scale(value):
    # FoliageWind::pack uses lround on a nonnegative value.
    scale = np.float32(4)
    byte = np.floor(np.clip(np.float32(value) / scale, 0, 1) * 255 + 0.5)
    return byte * (scale / np.float32(255))


def travel(positions, colors, reach, wind, flutter):
    # treeboughasset.cpp::vertexTravel and foliagetree.h::travel.
    tree_lean, sway_calm, sway_wind, tree_across = map(np.float32, (0.03, 0.001, 0.007, 0.35))
    branch_reach, branch_give, branch_top, branch_gust = map(np.float32, (0.3, 0.08, 1.25, 0.5))
    leaf_max, leaf_up, leaf_side = map(np.float32, (2, 0.8, 0.6))
    height = np.clip(positions[:, 1] / reach, 0, 1)
    branch = np.hypot(positions[:, 0], positions[:, 2]) / (reach * branch_reach)
    branch = np.where(colors[:, 0] > 0, np.minimum(1, branch), 0)
    leaf = colors[:, 2].astype(np.float32) / np.float32(255)
    trunk_travel = 2 * (tree_lean + (sway_calm + sway_wind) * (1 + tree_across))
    branch_travel = branch_reach * branch_give * branch_top * (1 + branch_gust)
    return reach * wind * (trunk_travel * (height * height) + branch_travel * branch) + (
        flutter * leaf_max * (leaf_up + leaf_side) * leaf
    )


def foliage_config(document, primitive, settings):
    name = document['materials'][primitive['material']]['name']
    return settings[name if Path(name).suffix else f'{name}.TGA']['foliage']


def load_wind_bounds(asset_json: Path, base_mesh: Path, materials: Path):
    """Return the residual B1 padding and the authored crown envelope in local cm."""
    asset = json.loads(asset_json.read_bytes())
    if hashlib.sha256(base_mesh.read_bytes()).hexdigest() != asset['source']['source']:
        raise ValueError('The base GLB does not match the hierarchy source')
    settings = json.loads(materials.read_bytes())
    base, base_binary = read_glb(base_mesh)
    base_primitives = base['meshes'][0]['primitives']
    base_positions = [
        accessor(base, base_binary, primitive['attributes']['POSITION']) * GLTF_TO_ENGINE
        for primitive in base_primitives
    ]
    reach = np.float32(max(points[:, 1].max() for points in base_positions))
    (base_crown, crown_positions), = [
        (primitive, points)
        for primitive, points in zip(base_primitives, base_positions, strict=True)
        if base['materials'][primitive['material']].get('alphaMode') == 'MASK'
        and foliage_config(base, primitive, settings)['anchor'] == 'tree'
    ]

    document, binary = read_glb(asset_json.with_name(asset['glb']))
    nodes = asset['nodes']
    if asset['axes'] != 'Z-up metres' or asset['wood_primitive'] != 0:
        raise ValueError('Expected the spruce generator axes with wood in primitive 0')
    box = np.array([node['cull_box'] for node in nodes], dtype=np.float32)
    box = box[:, :, [0, 2, 1]] * np.array([-100, 100, -100], dtype=np.float32)
    lo, hi = box.min(axis=1), box.max(axis=1)
    root_ids = set(asset['roots'])
    roots = [i for i, node in enumerate(nodes) if node['id'] in root_ids]

    padding = np.float32(0)
    primitives = document['meshes'][0]['primitives']
    for page, primitive in enumerate(primitives[1:], 1):
        attrs = primitive['attributes']
        positions = accessor(document, binary, attrs['POSITION']) * GLTF_TO_ENGINE
        colors = accessor(document, binary, attrs['COLOR_0'])
        if colors.shape != (len(positions), 4) or colors.dtype != np.uint8:
            raise ValueError('Expected four COLOR_0 bytes per foliage vertex')
        foliage = foliage_config(document, primitive, settings)
        if foliage['anchor'] != 'tree':
            raise ValueError('Expected the tree wind model on the spruce material')
        if page == 1:
            base_colors = accessor(base, base_binary, base_crown['attributes']['COLOR_0'])
            if (not np.array_equal(positions, crown_positions) or not np.array_equal(colors, base_colors)
                    or foliage != foliage_config(base, base_crown, settings)):
                raise ValueError('The source foliage does not match the original GLB')
        movement = travel(positions, colors, reach, packed_scale(foliage['wind']),
                          packed_scale(foliage['flutter']))
        for i, node in enumerate(nodes):
            geometry = node['geometry']
            if geometry is None or geometry['primitive'] != page:
                continue
            first = geometry['first_vertex']
            vertices = slice(first, first + geometry['vertex_count'])
            points = positions[vertices]
            margin = np.minimum(points - lo[i], hi[i] - points).min(axis=1)
            if (margin < -BOUNDS_TOLERANCE_CM).any():
                raise ValueError(f'Vertex outside the box of node {node["id"]}')
            padding = max(padding, (movement[vertices] - margin).max())

    # Every accepted parent can miss its child's box by the loader tolerance.
    padding += MAX_DEPTH * BOUNDS_TOLERANCE_CM
    crown_lo = np.minimum(lo[roots].min(axis=0), crown_positions.min(axis=0))
    crown_hi = np.maximum(hi[roots].max(axis=0), crown_positions.max(axis=0))
    return float(padding), crown_lo, crown_hi
