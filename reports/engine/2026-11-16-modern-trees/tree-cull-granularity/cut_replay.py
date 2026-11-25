"""Replay the compact spruce cut after the recorded crown visibility decision."""

import json
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np


@dataclass(frozen=True)
class Cut:
    lo: np.ndarray
    hi: np.ndarray
    parent: np.ndarray
    end: np.ndarray
    triangles: np.ndarray
    primitive: np.ndarray
    mesh_class: np.ndarray
    roots: np.ndarray
    diameter: np.ndarray
    low_px: np.ndarray
    high_px: np.ndarray
    open_scale: float
    source_triangles: int
    canopy_lo: np.ndarray
    canopy_hi: np.ndarray
    drawn_lo: np.ndarray
    drawn_hi: np.ndarray


@dataclass(frozen=True)
class Replay:
    selected: np.ndarray
    walk_visits: np.ndarray
    probed: int
    walked: int
    compact: bool
    triangles: int


def glb_primitives(path):
    with path.open('rb') as file:
        magic, version, _, size, kind = struct.unpack('<4sIII4s', file.read(20))
        if (magic, version, kind) != (b'glTF', 2, b'JSON'):
            raise ValueError(f'Expected a GLB v2 JSON chunk: {path}')
        document = json.loads(file.read(size))
    accessors = document['accessors']
    primitives = document['meshes'][0]['primitives']
    if len(document['meshes']) != 1 or document['nodes'][0].keys() - {'mesh', 'name'}:
        raise ValueError(f'Expected one untransformed mesh: {path}')
    return [(accessors[p['attributes']['POSITION']], accessors[p['indices']]) for p in primitives]


def load_cut(asset_json: Path, level_glbs=()):
    data = json.loads(asset_json.read_bytes())
    if data['axes'] != 'Z-up metres' or data['wood_primitive'] != 0:
        raise ValueError('Unexpected hierarchy axes or source primitives')
    primitives = glb_primitives(asset_json.with_name(data['glb']))
    records = {record['id']: record for record in data['nodes']}
    ordered, parents, ends, roots = [], [], [], []

    def visit(node_id, parent):
        record = records.pop(node_id)
        index = len(ordered)
        ordered.append(record)
        parents.append(parent)
        ends.append(0)
        for child in record['children']:
            visit(child, index)
        ends[index] = len(ordered)

    for node_id in data['roots']:
        roots.append(len(ordered))
        visit(node_id, -1)
    if records:
        raise ValueError('Unreachable hierarchy nodes')

    def bounds(field):
        # treeboughasset.cpp: producer Z-up metres to mirrored engine centimetres.
        points = np.array([record[field] for record in ordered], dtype=np.float32)
        points = points[:, :, [0, 2, 1]] * np.array([-100, 100, -100], dtype=np.float32)
        return np.minimum(points[:, 0], points[:, 1]), np.maximum(points[:, 0], points[:, 1])

    source_lo, source_hi = bounds('box')
    lo, hi = bounds('cull_box')
    diameter = np.linalg.norm(source_hi - source_lo, axis=1)
    low_px = np.array([record['low_px'] for record in ordered], dtype=np.float32)
    high_px = np.array([record['high_px'] for record in ordered], dtype=np.float32)
    triangles = np.zeros(len(ordered), dtype=np.int32)
    primitive = np.zeros_like(triangles)
    mesh_class = np.zeros_like(triangles)
    ranges = [[] for _ in primitives]
    for i, record in enumerate(ordered):
        geometry = record['geometry']
        if geometry is None:
            continue
        page = geometry['primitive']
        indices = geometry['index_count']
        vertices = geometry['vertex_count']
        if not 0 < page < len(primitives) or indices % 3:
            raise ValueError(f'Invalid GLB range for node {record["id"]}')
        ranges[page].append((geometry['first_index'], indices, geometry['first_vertex'], vertices))
        if page == 1 or high_px[i] > 0:
            triangles[i] = indices // 3
            primitive[i] = page
            # treeboughgroups.h: rounded down to a power of two.
            fit = 64 // max(vertices, triangles[i], 8)
            mesh_class[i] = 1 << (int(fit).bit_length() - 1)
    for (positions, indices), page in zip(primitives[1:], ranges[1:], strict=True):
        next_index = next_vertex = 0
        for first_index, index_count, first_vertex, vertex_count in sorted(page):
            if (first_index, first_vertex) != (next_index, next_vertex):
                raise ValueError('Hierarchy ranges do not partition GLB geometry')
            next_index += index_count
            next_vertex += vertex_count
        if (next_index, next_vertex) != (indices['count'], positions['count']):
            raise ValueError('Hierarchy ranges do not match the GLB counts')

    canopy_lo = lo[roots].min(axis=0)
    canopy_hi = hi[roots].max(axis=0)
    drawn_lo = canopy_lo.copy()
    drawn_hi = canopy_hi.copy()
    # draw bounds include the original mesh, all levels and the canopies' proxy bounds.
    for positions, _ in [*primitives[:2], *(p for path in level_glbs for p in glb_primitives(path))]:
        corners = np.array([positions['min'], positions['max']], dtype=np.float32)
        corners *= np.array([-100, 100, 100], dtype=np.float32)
        drawn_lo = np.minimum(drawn_lo, corners.min(axis=0))
        drawn_hi = np.maximum(drawn_hi, corners.max(axis=0))
    return Cut(lo, hi, np.array(parents, dtype=np.int32), np.array(ends, dtype=np.int32), triangles,
               primitive, mesh_class, np.array(roots, dtype=np.int32), diameter, low_px, high_px,
               np.max(high_px / diameter), primitives[1][1]['count'] // 3,
               canopy_lo, canopy_hi, drawn_lo, drawn_hi)


def replay_cut(cut, object_matrix, view, focal, near):
    """Mirror the 64 shader slices and count hierarchy reads in the probe and walk."""
    obj = np.asarray(object_matrix, dtype=np.float32)
    view = np.asarray(view, dtype=np.float32)
    basis = obj[:3, :3]
    gram = basis.T @ basis
    scale = np.sqrt(np.abs(gram).sum(axis=0).max())
    forward = view[2, :3] @ basis
    origin_depth = view[2, :3] @ obj[:3, 3] + view[2, 3]
    focal_scale = np.float32(focal) * scale
    far = np.where(forward < 0, cut.drawn_lo, cut.drawn_hi)
    far_depth = origin_depth + forward @ far
    count = len(cut.end)
    ids = np.arange(count, dtype=np.int32)
    leaf = cut.end == ids + 1
    walk_visits = np.zeros(count, dtype=np.int32)
    starts = range(0, count, (count + 63) // 64)
    if far_depth <= near or focal_scale > far_depth * cut.open_scale:
        return Replay(ids[leaf], walk_visits, 0, 0, False, cut.source_triangles)

    active = (cut.high_px > 0) & ~leaf
    weights = np.zeros(count, dtype=np.float32)
    closest = np.where(forward < 0, cut.hi[active], cut.lo[active])
    depth = origin_depth + (closest * forward).sum(axis=1)
    eligible = depth > near
    size = np.full(len(depth), np.inf, dtype=np.float32)
    np.divide(cut.diameter[active] * focal_scale, depth, out=size, where=eligible)
    t = np.clip((size - cut.low_px[active]) / (cut.high_px[active] - cut.low_px[active]), 0, 1)
    weights[active] = 1 - t * t * (3 - 2 * t)
    probed = 0
    any_merge = False
    for first in starts:
        last = min(first + starts.step, count)
        hits = np.flatnonzero(weights[first:last] > 0)
        stop = first + int(hits[0]) + 1 if hits.size else last
        probed += stop - first
        any_merge |= bool(hits.size)
    if not any_merge:
        return Replay(ids[leaf], walk_visits, probed, 0, False, cut.source_triangles)

    selected = []
    ends = cut.end.tolist()
    parents = cut.parent.tolist()
    weights = weights.tolist()
    active = active.tolist()
    visits = walk_visits.tolist()
    for first in starts:
        last = min(first + starts.step, count)
        band_end = below_end = proxy_end = 0
        band_weight = 0
        visits[first] += 1  # Initial read to find the first node's parent.
        parent = parents[first]
        while parent >= 0:
            visits[parent] += 1
            if active[parent]:
                if weights[parent] > 0:
                    band_end = ends[parent]
                    band_weight = weights[parent]
                    below_end = proxy_end
                proxy_end = ends[parent]
            parent = parents[parent]
        if band_weight >= 1:
            i = band_end
            band_end = 0
        else:
            i = max(first, below_end)
        while i < last:
            visits[i] += 1
            end = ends[i]
            if end == i + 1:
                selected.append(i)
            elif active[i]:
                if i < band_end or weights[i] >= 1:
                    selected.append(i)
                    i = end
                    continue
                if weights[i] > 0:
                    selected.append(i)
                    band_end = end
            i += 1
    selected = np.array(selected, dtype=np.int32)
    walk_visits = np.array(visits, dtype=np.int32)
    return Replay(selected, walk_visits, probed, int(walk_visits.sum()),
                  True, int(cut.triangles[selected].sum()))
