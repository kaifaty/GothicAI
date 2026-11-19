"""Disjoint foliage groups for the fir generator's offline LOD experiment."""
from __future__ import annotations

import json
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import numpy as np

import tree_glb
import tree_prefix

VERTEX_FIELDS = ('position', 'normal', 'uv', 'colour')


@dataclass
class Node:
    id: int
    kind: str
    children: tuple[int, ...]
    leaves: tuple[int, ...]
    branch: int
    rest_low: np.ndarray
    rest_high: np.ndarray
    low: np.ndarray
    high: np.ndarray
    first: int = 0
    count: int = 0
    first_index: int = 0
    index_count: int = 0
    max_px: float = 0.
    threshold_px: float = 0.
    high_px: float = 0.
    low_px: float = 0.

    @property
    def leaf_count(self) -> int:
        return len(self.leaves)

    @property
    def diameter(self) -> float:
        return float(np.linalg.norm(self.high - self.low))


def wind_travel(position: np.ndarray, colour: np.ndarray, reach: float) -> np.ndarray:
    """Full-wind bound per vertex, in metres at wind and flutter 1."""
    # FoliageTree::travel uses the same maxima over the whole tree. At a given
    # vertex, the trunk and branch arms and the flutter weight can be smaller.
    packed_scale = 64 * 4 / 255
    trunk = 2 * reach * np.clip(position[:, 2] / reach, 0, 1) ** 2 * (.03 + (.001 + .007) * (1 + .35))
    arm = np.minimum(np.linalg.norm(position[:, :2], axis=1), .3 * reach)
    branch = arm * (.08 * 1.25 * (1 + .5)) * (colour[:, 0] > 0)
    flutter = (colour[:, 2] / 255) * (.02 * (.8 + .6))
    return packed_scale * (trunk + branch + flutter)


def build(foliage: dict, tree, census: dict) -> tuple[list[Node], tuple[int, ...]]:
    """Keep source card order and attach each card to a single local root."""
    first, _, by_face = tree_prefix.cards(foliage)
    if not np.array_equal(first, [card['first'] for card in tree.cards]):
        raise ValueError('The hierarchy requires the original card ranges')
    if np.any(np.diff(by_face) < 0):
        raise ValueError('Expected contiguous triangle ranges for the original fir cards')
    vertex_counts = np.diff(np.append(first, len(foliage['position'])))
    triangle_counts = np.bincount(by_face, minlength=len(first))
    triangle_starts = np.cumsum(triangle_counts) - triangle_counts
    if np.any(triangle_counts == 0):
        raise ValueError('Each source card must have triangles')
    positions = foliage['position'].astype(np.float64)
    reach = max(float(positions[:, 2].max()), 1e-5)
    padding = wind_travel(positions, foliage['colour'], reach)[:, None]
    wind_low, wind_high = positions - padding, positions + padding
    nodes = []

    def append(kind, children, leaves, branch, rest_low, rest_high, low, high, **ranges):
        ident = len(nodes) + 1
        nodes.append(Node(ident, kind, children, leaves, branch, rest_low, rest_high, low, high,
                          **ranges))
        return ident

    def group(kind, ids, branch=0):
        if len(ids) == 1:
            return ids[0]
        children = [nodes[i - 1] for i in ids]
        return append(kind, tuple(ids), tuple(i for child in children for i in child.leaves), branch,
                      np.min([child.rest_low for child in children], axis=0),
                      np.max([child.rest_high for child in children], axis=0),
                      np.min([child.low for child in children], axis=0),
                      np.max([child.high for child in children], axis=0))

    def cell(z, azimuth):
        return math.floor(z / 1.5), math.floor(3 * (azimuth % (2 * math.pi)) / (2 * math.pi))

    limbs, clusters, stems, tiers = (defaultdict(list) for _ in range(4))
    branches = {branch.number: branch for branch in tree.branches[tree.stubs:]}
    for i, card in enumerate(tree.cards):
        start, count = int(first[i]), int(vertex_counts[i])
        if card.get('count', count) != count:
            raise ValueError('The hierarchy requires complete original vertex ranges')
        vertices = slice(start, start + count)
        low, high = positions[vertices].min(axis=0), positions[vertices].max(axis=0)
        branch = card['branch']
        ident = append('leaf', (), (i + 1,), branch, low, high, wind_low[vertices].min(axis=0),
                       wind_high[vertices].max(axis=0), first=start, count=count,
                       first_index=int(3 * triangle_starts[i]), index_count=int(3 * triangle_counts[i]))
        if branch and card['cluster']:
            kind = 'hang' if card['role'] == 'hang' else 'end'
            clusters[branch, kind, card['cluster']].append(ident)
        elif branch:
            limbs[branch].append(ident)
        else:
            x, y, z = (low + high) * .5
            stems[cell(z, math.atan2(y, x)), card['role']].append(ident)

    for (branch, kind, _), ids in clusters.items():
        limbs[branch].append(group(kind, ids, branch))
    for number, ids in sorted(limbs.items()):
        branch = branches[number]
        key = cell(branch.points[0, 2] * census['scale'], branch.azimuth)
        tiers[key].append(group('bough', ids, number))
    for (key, kind), ids in sorted(stems.items()):
        for i in range(0, len(ids), 4):
            tiers[key].append(group(kind, ids[i:i + 4]))
    roots = tuple(group('tier', ids) for _, ids in sorted(tiers.items()))
    return nodes, roots


def leaf_part(foliage: dict, nodes: list[Node], ids) -> dict:
    """Copy a set of original cards in their original order, reindexing their faces."""
    ids = sorted(ids)
    if len(ids) != len(set(ids)) or any(i < 1 or i > len(nodes) or nodes[i - 1].children for i in ids):
        raise ValueError('Expected distinct source card IDs')
    part = dict(foliage)
    if not ids:
        return {**part, **{field: foliage[field][:0] for field in (*VERTEX_FIELDS, 'index')}}
    cards = [nodes[i - 1] for i in ids]
    for field in VERTEX_FIELDS:
        part[field] = np.concatenate([foliage[field][n.first:n.first + n.count] for n in cards])
    index = []
    start = 0
    for n in cards:
        first, count = n.first_index // 3, n.index_count // 3
        index.append(foliage['index'][first:first + count] + (start - n.first))
        start += n.count
    part['index'] = np.concatenate(index)
    return part


def export(path: Path, source_parts: list[dict], nodes: list[Node], roots: tuple[int, ...],
           parents: dict[int, dict], atlas, *, metadata: dict | None = None) -> dict:
    """Write a GLB with source parts first and atlas-page primitives after them."""
    wood, foliage = tree_prefix.split_parts(source_parts)
    original = [n for n in nodes if not n.children]
    path = Path(path)
    parts = [dict(wood, preserve_normals=True),
             dict(foliage, preserve_normals=True,
                  card_id=np.repeat([n.id for n in original], [n.count for n in original]))]
    ranges = {n.id: dict(primitive=1, first_vertex=n.first, vertex_count=n.count,
                         first_index=n.first_index, index_count=n.index_count) for n in original}

    geometry_bytes = 0
    page, batch, vertices, indices = None, [], 0, 0

    def flush():
        nonlocal vertices, indices, geometry_bytes
        if not batch:
            return
        name = f'FIR_HIERARCHY_{page}_{len(parts)}'
        part = dict(name=name, group='UNDEF', cutout=True, two_sided=True)
        for field in (*VERTEX_FIELDS, 'index'):
            part[field] = np.concatenate([item[field] for item in batch])
        parts.append(part)
        geometry_bytes += 36 * vertices + 2 * indices + (-2 * indices % 4)
        batch.clear()
        vertices = indices = 0

    for ident, part in sorted(parents.items(), key=lambda item: (item[1]['page'], item[0])):
        if not 1 <= ident <= len(nodes) or not nodes[ident - 1].children:
            raise ValueError(f'Proxy geometry must belong to a parent node, got {ident}')
        count, index_count = len(part['position']), part['index'].size
        if count > 65535 or not count or not index_count:
            raise ValueError(f'Node {ident} has an unsupported proxy geometry size')
        if page != part['page'] or vertices + count > 65535:
            flush()
            page = part['page']
        ranges[ident] = dict(primitive=len(parts), page=page, first_vertex=vertices, vertex_count=count,
                             first_index=indices, index_count=index_count)
        batch.append(dict(part, index=part['index'] + vertices))
        vertices += count
        indices += index_count
    flush()

    reach = max(float(foliage['position'][:, 2].max()), 1e-5)
    records, low, high = [], {}, {}
    for node in nodes:
        lo, hi = node.low.copy(), node.high.copy()
        for child in node.children:
            lo, hi = np.minimum(lo, low[child]), np.maximum(hi, high[child])
        if node.id in parents:
            proxy = parents[node.id]
            position = proxy['position'].astype(np.float64)
            padding = wind_travel(position, proxy['colour'], reach)[:, None]
            lo = np.minimum(lo, (position - padding).min(axis=0))
            hi = np.maximum(hi, (position + padding).max(axis=0))
        low[node.id], high[node.id] = lo, hi
        records.append(dict(id=node.id, kind=node.kind, branch=node.branch, children=node.children,
                            source_cards=node.leaf_count, box=[node.low.tolist(), node.high.tolist()],
                            cull_box=[lo.tolist(), hi.tolist()],
                            sphere=[*((lo + hi) * .5).tolist(), float(np.linalg.norm(hi - lo) * .5)],
                            max_px=node.max_px, threshold_px=node.threshold_px,
                            high_px=node.high_px, low_px=node.low_px, geometry=ranges.get(node.id)))

    sidecar = path.with_suffix('.json')
    tree_glb.write(path, parts, extras={'hierarchy': sidecar.name})
    output = dict(glb=path.name, axes='Z-up metres', roots=roots, wood_primitive=0, foliage_reach_m=reach,
                  atlas=atlas, nodes=records)
    if metadata:
        output['source'] = metadata
    sidecar.write_text(json.dumps(output, separators=(',', ':')) + '\n')
    return dict(geometry_bytes=geometry_bytes, glb_bytes=path.stat().st_size,
                hierarchy_bytes=sidecar.stat().st_size)
