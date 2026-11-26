"""Bake opaque UV contour meshlets for the four authored spruce card LODs.

    uv run --no-project --with numpy --with pillow --with scipy --with contourpy --with 'shapely>=2.1' \
        python scripts/content/mesh3d/blender/fir_nanite_contours.py ACCEPTED.glb BOUGHS-ALBEDO.dds contours.bin

The distant tree pilot selects the chart's mip after projecting each posed source face.
The cache uses the same contour for faces with identical ordered UV triples.
Each vertex is a barycentric point on that face, retaining its skin and material.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from array import array
from itertools import pairwise
from pathlib import Path
from tempfile import NamedTemporaryFile

import contourpy
import fir_nanite
import fir_tree
import numpy as np
import shapely
import tree_glb
import tree_hierarchy_raster as raster
import tree_prefix
from shapely.geometry import Polygon

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from impostor_bake import dds_mips

MIP_BASE = 3 * 32
MIP_COUNT = 8 * 32 + 1
MAX_VERTICES = MAX_TRIANGLES = 64


def outline(cell, side, mips, lod):
    x, y, width, height = cell['cell']
    step = 2 ** (int(lod) - 3)
    cx = np.r_[0, np.arange(step / 2, width, step), width]
    cy = np.r_[0, np.arange(step / 2, height, step), height]
    u, v = np.meshgrid((x + cx) / side, (y + cy) / side)
    alpha = raster._sample_mips(mips, u.ravel(), v.ravel(), np.full(u.size, lod))
    generator = contourpy.contour_generator(x=cx, y=cy, z=alpha.reshape(u.shape), fill_type='OuterOffset')
    curves, offsets = generator.filled(.5, 2.)
    return shapely.union_all([Polygon(curve[:ends[1]], [curve[a:b] for a, b in pairwise(ends[1:])])
                              .simplify(.05 * step) for curve, ends in zip(curves, offsets)])


def chart_triangles(corners, shape):
    matrix = (corners[1:] - corners[:1]).T
    winding = np.linalg.det(matrix)
    if abs(winding) < 1e-6 or shape.is_empty:
        return np.empty((0, 3), dtype=np.uint32)
    cut = shape.intersection(Polygon(corners))
    patch = shapely.get_coordinates(shapely.constrained_delaunay_triangles(cut)).reshape(-1, 4, 2)[:, :3]
    flip = np.linalg.det(patch[:, 1:] - patch[:, :1]) * winding < 0
    patch[flip] = patch[flip, ::-1]
    uv = np.linalg.solve(matrix, (patch.reshape(-1, 2) - corners[0]).T).T
    weights = np.rint(np.clip(uv, 0., 1.) * 65535.).astype(np.int64).reshape(-1, 3, 2)
    weights[:, :, 0] -= np.maximum(0, weights.sum(axis=2) - 65535)
    a, b = np.moveaxis(weights[:, 1:] - weights[:, :1], 1, 0)
    # Rounding can collapse or invert tiny slivers on the contour.
    keep = a[:, 0] * b[:, 1] > a[:, 1] * b[:, 0]
    points = weights[keep].astype(np.uint32)
    return points[:, :, 0] | points[:, :, 1] << 16


class Meshlets:
    def __init__(self):
        self.records, self.vertices, self.triangles = (array('I') for _ in range(3))

    def append(self, triangles):
        start = len(self.records) // 4
        vertices, indices = {}, []

        def flush():
            if indices:
                self.records.extend((len(self.vertices), len(self.triangles), len(vertices), len(indices)))
                self.vertices.extend(vertices)
                self.triangles.extend(indices)
                vertices.clear()
                indices.clear()

        for triangle in triangles.tolist():
            additional = sum(vertex not in vertices for vertex in triangle)
            if len(indices) == MAX_TRIANGLES or len(vertices) + additional > MAX_VERTICES:
                flush()
            a, b, c = (vertices.setdefault(point, len(vertices)) for point in triangle)
            indices.append(a | b << 8 | c << 16)
        flush()
        return start, len(self.records) // 4 - start


def load_faces(reference, tree, atlas):
    source = tree_glb.read(reference)
    if len(source) != len(tree.parts) or any(
            not np.array_equal(part[name], expected[name]) for part, expected in zip(source, tree.parts)
            for name in ('position', 'index', 'uv', 'colour')):
        raise ValueError('Expected the accepted spruce A L0 source')
    charts, faces = {}, []
    for level in range(1, 5):
        bark, leaves = tree_glb.read(reference.with_stem(f'{reference.stem}_L{level}'))
        starts, _, by_face = tree_prefix.cards(leaves)
        cards = fir_nanite.source_cards(tree, source[1], leaves, starts)
        ordered_uv = leaves['uv'][leaves['index']].astype(np.float64) * atlas['size']
        chart_ids = np.empty(len(by_face), dtype='<u4')
        for i, owner in enumerate(by_face):
            kind = cards[owner]['kind']
            corners = ordered_uv[i] - atlas['cells'][kind]['cell'][:2]
            chart_ids[i] = charts.setdefault((kind, corners.tobytes()), len(charts))
        faces.append(np.column_stack((leaves['index'] + len(bark['position']), chart_ids)).astype('<u4'))
    return faces, [(kind, np.frombuffer(key, np.float64).reshape(3, 2)) for kind, key in charts]


def bake(reference, albedo, output):
    config = json.loads((fir_nanite.HERE / 'fir.json').read_text())
    atlas = config['atlas']
    faces, charts = load_faces(reference, fir_tree.build(config), atlas)
    mips = [level[:, :, 3:4].astype(np.float32) / 255. for level in dds_mips(albedo)]
    if mips[0].shape[:2] != (atlas['size'], atlas['size']) or len(mips) != 12:
        raise ValueError('Expected the spruce atlas and its full stored mip chain')
    shapes = Meshlets()
    ranges = np.zeros((len(charts), MIP_COUNT, 2), dtype='<u4')
    for bin_index in range(MIP_COUNT):
        lod = (MIP_BASE + bin_index) / 32.
        outlines = {kind: outline(cell, atlas['size'], mips, lod) for kind, cell in atlas['cells'].items()}
        for chart, (kind, corners) in enumerate(charts):
            ranges[chart, bin_index] = shapes.append(chart_triangles(corners, outlines[kind]))
    counts = (len(charts), len(shapes.records) // 4, len(shapes.vertices), len(shapes.triangles))
    header = struct.pack('<4s11I', b'FCTL', 1, MIP_BASE, MIP_COUNT, *counts, *(len(level) for level in faces))
    if sys.byteorder != 'little' or shapes.records.itemsize != 4:
        raise ValueError('The contour writer requires little-endian 32-bit unsigned ints')
    output.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=output.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(header)
            for section in (*faces, ranges, shapes.records, shapes.vertices, shapes.triangles):
                section.tofile(stream)
            stream.flush()
            temporary.replace(output)
        finally:
            temporary.unlink(missing_ok=True)
    print(json.dumps(dict(zip(('charts', 'meshlets', 'vertices', 'triangles'), counts)), indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('reference', type=Path)
    parser.add_argument('albedo', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    bake(args.reference, args.albedo, args.output)
