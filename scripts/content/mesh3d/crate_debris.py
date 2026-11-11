#!/usr/bin/env python3
"""Debris of the harbour crate: model.debris.glb of its boards, from the baked model.

    uv run --no-project --with numpy --with pillow python scripts/content/mesh3d/crate_debris.py \
        BAKED.glb ALBEDO.png OUT.debris.glb [--seed 7]

Why and how: reports/content/2026-11-wood-fracture/README.md. A broken crate falls apart into the
parts it is made of, not into solid lumps. The baked crate (rigging_set.py bake) is one mesh: every
plank, batten and post is its own closed box with its own atlas island on each of its six faces, the
nail heads are open three-sided caps, and a dark core box fills the inside. This script

- keeps every box as one node (one piece) and drops the core, so the broken crate is hollow;
- puts each nail head on the board it sits on (render only, the cutter gives it no hull);
- by the fixed seed splits 3-4 planks along the grain into 2-3 strips (a long, nearly straight split,
  slightly slanted through the thickness) and breaks 1-2 planks across with a torn edge: teeth of
  2-4 cm and one long splinter, the tension face torn further than the compression face.

UV stay inside the same board's islands, never stretched: a split face takes the mapping of the long
edge of its board, a break facet the mapping of the board's end, laid along the facet in the board's
frame. The back faces and some edges and ends of the baked atlas are dark (ambient occlusion of the
core and the neighbours); such a face takes its opposite face's mapping, else a strip of the board's
front face of the same size. The glTF world, materials and extras are the baked model's (metres, +Y up).
The same inputs and seed give the same bytes.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct

import numpy as np

DARK = 0.004            # mean linear luminance of an island below which it reads as a black patch
SPLIT_WANDER = 0.0015   # m, sideways wander of a split along the grain
SPLIT_RUNOUT = 0.012    # m, the most a split drifts across the board from end to end (grain runout)
SPLIT_SLANT = 0.002     # m, offset of the split between the two faces of the board
TOOTH = (0.02, 0.04)    # m, peak-to-valley of the torn edge
TOOTH_PITCH = 0.04      # m, a valley and a peak across the board
SPLINTERS = 1           # long thin teeth; a second one (into the other half) gives up to 18 hulls a half
SPLINTER = (0.05, 0.08) # m, length of the splinter beyond the torn edge
BREAK_SLANT = (0.0015, 0.003)  # m, the tension face torn further: at most 1% stretch of the end's uv on a facet
NAIL_CLEAR = 0.015      # m, a split or a break keeps this far from a nail head


# --- GLB in and out ---------------------------------------------------------------------------------------------

def read_glb(path: Path):
    data = path.read_bytes()
    length = struct.unpack_from('<I', data, 12)[0]
    doc = json.loads(data[20:20 + length])
    blob = data[20 + length + 8:]

    def accessor(index):
        a = doc['accessors'][index]
        view = doc['bufferViews'][a['bufferView']]
        dtype = {5126: '<f4', 5123: '<u2', 5125: '<u4'}[a['componentType']]
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[a['type']]
        offset = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        return np.frombuffer(blob, dtype, a['count'] * width, offset).reshape(a['count'], width)

    primitives = []
    for mesh in doc['meshes']:
        for prim in mesh['primitives']:
            at = prim['attributes']
            primitives.append(dict(material=prim['material'], position=accessor(at['POSITION']).astype(np.float64),
                                   normal=accessor(at['NORMAL']).astype(np.float64),
                                   uv=accessor(at['TEXCOORD_0']).astype(np.float64),
                                   triangles=accessor(prim['indices']).reshape(-1, 3).astype(np.int64)))
    if len(doc.get('nodes', [])) != 1 or any(k in doc['nodes'][0] for k in ('matrix', 'rotation', 'scale', 'translation')):
        raise ValueError(f'{path}: expected one untransformed node')
    return doc, primitives


def write_glb(path: Path, materials: list[dict], nodes: list[tuple[str, list[tuple[int, np.ndarray, np.ndarray, np.ndarray]]]]):
    """nodes: [(name, [(material, positions (n,3), normals (n,3), uv (n,2)) per primitive, three vertices a triangle])]."""
    blob = bytearray()
    views, accessors, meshes, gltf_nodes = [], [], [], []

    def add(array, kind, target, minmax=False):
        data = np.ascontiguousarray(array).tobytes()
        blob.extend(b'\0' * (-len(blob) % 4))
        views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(data), target=target))
        blob.extend(data)
        accessor = dict(bufferView=len(views) - 1, componentType=5125 if array.dtype == np.uint32 else 5126,
                        count=len(array), type=kind)
        if minmax:
            accessor['min'] = [float(x) for x in array.min(0)]
            accessor['max'] = [float(x) for x in array.max(0)]
        accessors.append(accessor)
        return len(accessors) - 1

    for name, primitives in nodes:
        prims = []
        for material, position, normal, uv in primitives:
            # weld identical corners of a face (same position, normal and uv) into indexed vertices
            rows = np.hstack((position, normal, uv)).astype(np.float32)
            unique, inverse = np.unique(rows, axis=0, return_inverse=True)
            first = np.full(len(unique), len(rows))
            np.minimum.at(first, inverse.ravel(), np.arange(len(rows)))
            order = np.argsort(first)               # vertices in their first use, not sorted by value
            rank = np.empty_like(order)
            rank[order] = np.arange(len(order))
            vertices = unique[order]
            indices = rank[inverse.ravel()].astype(np.uint32)
            prims.append(dict(attributes=dict(POSITION=add(vertices[:, :3], 'VEC3', 34962, True),
                                              NORMAL=add(vertices[:, 3:6], 'VEC3', 34962),
                                              TEXCOORD_0=add(vertices[:, 6:8], 'VEC2', 34962)),
                              indices=add(indices, 'SCALAR', 34963), material=material, mode=4))
        meshes.append(dict(name=name, primitives=prims))
        gltf_nodes.append(dict(mesh=len(meshes) - 1, name=name))
    doc = dict(asset=dict(version='2.0', generator='scripts/content/mesh3d/crate_debris.py'),
               scene=0, scenes=[dict(name='debris', nodes=list(range(len(gltf_nodes))))], nodes=gltf_nodes,
               meshes=meshes, materials=materials, accessors=accessors, bufferViews=views,
               buffers=[dict(byteLength=len(blob))])
    text = json.dumps(doc, separators=(',', ':'), sort_keys=True).encode()
    text += b' ' * (-len(text) % 4)
    blob.extend(b'\0' * (-len(blob) % 4))
    path.write_bytes(struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(text) + 8 + len(blob)) +
                     struct.pack('<I4s', len(text), b'JSON') + text + struct.pack('<I4s', len(blob), b'BIN\0') + bytes(blob))


def components(position, triangles):
    """Triangle index lists of the position-connected parts, in the order of their first triangle."""
    _, weld = np.unique(position, axis=0, return_inverse=True)
    weld = weld.ravel()
    parent = list(range(weld.max() + 1))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for tri in weld[triangles]:
        for k in tri[1:]:
            a, b = root(int(tri[0])), root(int(k))
            parent[max(a, b)] = min(a, b)
    groups: dict[int, list[int]] = {}
    for index, tri in enumerate(weld[triangles]):
        groups.setdefault(root(int(tri[0])), []).append(index)
    return list(groups.values())


# --- boards -----------------------------------------------------------------------------------------------------

class Board:
    """A closed box of the baked crate: an affine frame and the uv map of each of its six faces.

    Local coordinates (l, w, n) run over [-1, 1]: l along the grain (the longest side), w across, n through
    the thickness. A face is (axis, sign), axis 0 = l, 1 = w, 2 = n; its map takes the two other local
    coordinates in axis order to uv."""

    def __init__(self, position, uv, triangles, albedo_lum):
        corners = np.unique(position[triangles].reshape(-1, 3), axis=0)
        if len(corners) != 8 or len(triangles) != 12:
            raise ValueError('a board must be a closed box of 12 triangles')
        self.centre = corners.mean(0)
        # the three edges at one corner: its nearest corner, the nearest off that line, the nearest off that plane
        others = corners[1:] - corners[0]
        others = others[np.argsort(np.linalg.norm(others, axis=1), kind='stable')]
        edges = [others[0]]
        for vector in others[1:]:
            basis = np.linalg.qr(np.array(edges).T)[0]
            rest = vector - basis @ (basis.T @ vector)
            if np.linalg.norm(rest) > 1e-3 * np.linalg.norm(vector):
                edges.append(vector)
                if len(edges) == 3:
                    break
        edges = np.array(edges)
        edges = edges[np.argsort(np.linalg.norm(edges, axis=1))[::-1]]   # length, width, thickness
        self.size = np.linalg.norm(edges, axis=1)
        self.matrix = edges.T / 2            # local (l, w, n) -> offset from the centre
        if np.linalg.det(self.matrix) < 0:   # keep the frame right-handed: n = l x w side
            self.matrix[:, 2] *= -1
        self.inverse = np.linalg.inv(self.matrix)
        self.maps, self.lum = {}, {}
        for tri in triangles:
            p = position[tri]
            local = self.local(p)
            axis = int(np.argmin(np.ptp(local, axis=0)))   # the coordinate shared by all three corners
            sign = int(np.sign(local[0, axis]))
            other = [k for k in range(3) if k != axis]
            rows = np.c_[local[:, other], np.ones(3)]
            self.maps.setdefault((axis, sign), []).append((rows, uv[tri]))
        for face, samples in list(self.maps.items()):
            rows = np.vstack([r for r, _ in samples])
            uvs = np.vstack([u for _, u in samples])
            affine = np.linalg.lstsq(rows, uvs, rcond=None)[0]
            if np.abs(rows @ affine - uvs).max() > 1e-5:
                raise ValueError('a board face has no affine uv map')
            self.maps[face] = affine
            self.lum[face] = albedo_lum(uvs)
        if len(self.maps) != 6:
            raise ValueError('a board must have six faces')
        # the front: the brighter big face; dark faces fall back on the opposite face, then on the front
        self.front = max(((2, 1), (2, -1)), key=lambda f: self.lum[f])

    def local(self, points):
        return (np.asarray(points) - self.centre) @ self.inverse.T

    def world(self, local):
        return self.centre + np.asarray(local) @ self.matrix.T

    def bright(self, axis, sign):
        """The face whose map a face (axis, sign) of this board shows: itself, its opposite or None (dark)."""
        for face in ((axis, sign), (axis, -sign)):
            if self.lum[face] >= DARK:
                return face
        return None

    def uv(self, face, coords):
        return np.c_[coords, np.ones(len(coords))] @ self.maps[face]

    def front_strip(self, axis, coords, centre):
        """A dark edge (axis 1) or end (axis 0) shown on a strip of the front of the same size: its (l, n) or
        (w, n) laid on the front around l = centre or w = centre, thickness along the other coordinate."""
        t = self.size[2] / self.size[axis]              # thickness in units of the front's coordinate it lies on
        centre = float(np.clip(centre, -1 + t, 1 - t))
        a, n = coords[:, 0], coords[:, 1]
        if axis == 1:                                    # an edge: (l, n) -> front (l, centre + n t)
            return self.uv(self.front, np.c_[a, centre + n * t])
        return self.uv(self.front, np.c_[centre + n * t, a])  # an end: (w, n) -> front (centre + n t, w)


def board_name(board: Board, crate_centre) -> tuple[str, float]:
    """A part name by role, and the key that orders parts of one role."""
    length, thick = board.matrix[:, 0] * 2, board.matrix[:, 2] * 2
    c = board.centre - crate_centre
    if abs(length[1]) > 0.5 * np.linalg.norm(length):     # upright: posts and battens
        if board.size[2] >= 0.03:
            return f'post-{"a" if c[0] < 0 else "b"}{"l" if c[2] < 0 else "r"}', 0.0
        return f'batten-{"a" if c[2] < 0 else "b"}', 0.0
    axis = int(np.argmax(np.abs(thick)))
    if axis == 1:
        return 'lid', c[2]
    if axis == 2:
        return f'plank-side-{"a" if c[2] < 0 else "b"}', c[1]
    return f'plank-end-{"a" if c[0] < 0 else "b"}', c[1]


# --- pieces of a board ------------------------------------------------------------------------------------------

def earclip(points):
    """Triangles (index triples) of a simple counter-clockwise polygon."""
    index = list(range(len(points)))
    triangles = []

    def cross(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])

    while len(index) > 3:
        for k in range(len(index)):
            a, b, c = index[k - 1], index[k], index[(k + 1) % len(index)]
            if cross(points[a], points[b], points[c]) <= 1e-12:
                continue
            if any(cross(points[a], points[b], points[j]) >= -1e-12 and cross(points[b], points[c], points[j]) >= -1e-12
                   and cross(points[c], points[a], points[j]) >= -1e-12
                   for j in index if j not in (a, b, c)):
                continue
            triangles.append((a, b, c))
            index.pop(k)
            break
        else:
            raise ValueError('polygon has no ear')
    triangles.append(tuple(index))
    return triangles


class Piece:
    """A piece of a board: a top outline (n = +1) and a bottom outline (n = -1) in (l, w), counter-clockwise,
    vertex for vertex; each side i -> i+1 has a kind: ('face', axis, sign), ('split', w) or ('break', l)."""

    def __init__(self, name, board, top, bottom, sides):
        self.name, self.board = name, board
        self.top, self.bottom, self.sides = np.asarray(top, float), np.asarray(bottom, float), sides
        self.nails = []

    def contains(self, lw):
        """Whether (l, w) lies in the top outline (even-odd)."""
        inside = False
        for (l0, w0), (l1, w1) in zip(self.top, np.roll(self.top, -1, axis=0)):
            if (w0 > lw[1]) != (w1 > lw[1]) and lw[0] < l0 + (lw[1] - w0) * (l1 - l0) / (w1 - w0):
                inside = not inside
        return inside

    def triangles(self):
        """[(world positions (3,3), uv (3,2))] of the closed piece, wound outward."""
        board = self.board
        out = []
        for outline, n, flip in ((self.top, 1.0, False), (self.bottom, -1.0, True)):
            face = board.bright(2, int(n)) or board.front
            for tri in earclip(outline):
                tri = tri[::-1] if flip else tri
                lw = outline[list(tri)]
                out.append((np.c_[lw, np.full(3, n)], board.uv(face, lw)))
        count = len(self.top)
        for i in range(count):
            j = (i + 1) % count
            quad = np.array([[*self.top[i], 1], [*self.bottom[i], -1], [*self.bottom[j], -1], [*self.top[j], 1]])
            uv = self.side_uv(self.sides[i], quad)
            for a, b, c in ((0, 1, 2), (0, 2, 3)):
                if np.linalg.norm(np.cross(board.world(quad[b]) - board.world(quad[a]),
                                           board.world(quad[c]) - board.world(quad[a]))) > 1e-12:
                    out.append((quad[[a, b, c]], uv[[a, b, c]]))
        return [(board.world(local), uv) for local, uv in out]

    def side_uv(self, kind, quad):
        board = self.board
        if kind[0] == 'face':
            _, axis, sign = kind
            coords = quad[:, [k for k in range(3) if k != axis]]
            face = board.bright(axis, sign)
            if face is not None:
                return board.uv(face, coords)
            return board.front_strip(axis, coords, quad[:, axis].mean())
        if kind[0] == 'split':        # a face along the grain: the long edge's map, (l, n)
            coords = quad[:, [0, 2]]
            face = board.bright(1, 1) if board.lum[1, 1] >= board.lum[1, -1] else board.bright(1, -1)
            return board.uv(face, coords) if face else board.front_strip(1, coords, kind[1])
        # a break facet: the end's map laid along the facet, (t, n) with t the distance along the facet
        top = quad[[0, 3], :2] * board.size[:2] / 2
        direction = top[1] - top[0]
        span = np.linalg.norm(direction)
        direction = direction / span if span > 1e-9 else np.array([0.0, 1.0])
        t = (quad[:, :2] * board.size[:2] / 2 - top[0]) @ direction
        half_w = board.size[1] / 2
        # centred where the facet lies across the board, kept inside the end
        start = float(np.clip(quad[:, 1].mean() * half_w - (t.max() + t.min()) / 2, -half_w - t.min(), half_w - t.max()))
        coords = np.c_[(start + t) / half_w, quad[:, 2]]
        face = board.bright(0, 1) if board.lum[0, 1] >= board.lum[0, -1] else board.bright(0, -1)
        return board.uv(face, coords) if face else board.front_strip(0, coords, kind[1])


def whole(name, board):
    square = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
    return [Piece(name, board, square, square, [('face', 1, -1), ('face', 0, 1), ('face', 1, 1), ('face', 0, -1)])]


def split(name, board, rng, strips, nails_lw):
    """Strips along the grain: each split line is nearly straight, drifts across the board a little from end
    to end and runs slanted through the thickness; both strips share it exactly."""
    half_w = board.size[1] / 2
    clear = NAIL_CLEAR / half_w
    for _ in range(200):
        cuts = np.sort(rng.uniform(-0.55, 0.55, strips - 1)) if strips == 2 else np.sort(rng.uniform(-0.7, 0.7, strips - 1))
        if np.all(np.diff(np.r_[-1, cuts, 1]) >= 2 * 0.04 / board.size[1]) and \
                all(abs(w - c) > clear for _, w in nails_lw for c in cuts):
            break
    else:
        raise ValueError(f'{name}: no split clear of the nails')
    steps = np.linspace(-1, 1, 9)
    lines = [(np.c_[steps, -np.ones(9)], np.c_[steps, -np.ones(9)], ('face', 1, -1))]
    for c in cuts:
        drift = rng.uniform(-SPLIT_RUNOUT, SPLIT_RUNOUT) / half_w
        wander = rng.uniform(-SPLIT_WANDER, SPLIT_WANDER, 9) / half_w
        top = np.c_[steps, c + drift * steps / 2 + wander]
        bottom = top + [0, rng.choice([-1, 1]) * SPLIT_SLANT / half_w]
        lines.append((top, bottom, ('split', c)))
    lines.append((np.c_[steps, np.ones(9)], np.c_[steps, np.ones(9)], ('face', 1, 1)))
    pieces = []
    for k in range(strips):
        (lo_t, lo_b, lo_kind), (hi_t, hi_b, hi_kind) = lines[k], lines[k + 1]
        top = [*lo_t, *hi_t[::-1]]
        bottom = [*lo_b, *hi_b[::-1]]
        sides = [lo_kind] * 8 + [('face', 0, 1)] + [hi_kind] * 8 + [('face', 0, -1)]
        pieces.append(Piece(f'{name}-strip-{k + 1}', board, top, bottom, sides))
    return pieces


def broken(name, board, rng, nails_lw):
    """Two halves of a plank broken across: a torn edge of narrow uneven teeth and a long splinter.

    The valleys of one half lie on one line and its peaks on another, the two lines at different slants,
    so the teeth grow from 2 to 4 cm across the board while every tooth and the body behind them stay
    convex: the cutter then gives a half one hull for its body and one for each tooth. An inner peak
    runs out into a narrow splinter. The tear runs slanted through the thickness by the same amount
    everywhere, so all its facets are flat."""
    half_l, half_w = board.size[0] / 2, board.size[1] / 2
    clear = (NAIL_CLEAR + TOOTH[1] + SPLINTER[1]) / half_l
    for _ in range(200):
        at = rng.uniform(-0.4, 0.4)
        if all(abs(l - at) > clear for l, _ in nails_lw):
            break
    else:
        raise ValueError(f'{name}: no break clear of the nails')
    count = 2 * max(4, int(round(board.size[1] / TOOTH_PITCH)))   # segments: valley-peak pairs
    w = np.linspace(-1, 1, count + 1)
    w[1:-1] += rng.uniform(-0.3, 0.3, count - 1) * (2 / count)
    # an inner peak (and with SPLINTERS = 2 an inner valley three segments away) becomes a splinter
    pairs = [(p, v) for p in range(3, count - 2, 2) for v in range(2, count - 1, 2) if abs(p - v) >= 3]
    splinters = pairs[int(rng.integers(len(pairs)))][:SPLINTERS]
    for k in splinters:
        width = rng.uniform(0.012, 0.018) / half_w
        w[k - 1], w[k + 1] = w[k] - width / 2, w[k] + width / 2       # a narrow tooth ...
    ends = rng.permutation([TOOTH[0], TOOTH[1]]) * rng.uniform(0.9, 1.0, 2)
    rise = (ends[0] + (ends[1] - ends[0]) * (w + 1) / 2) / half_l     # tooth height across the board
    tilt = rng.uniform(-0.015, 0.015) / half_l                         # the whole tear a little oblique
    l_top = at + tilt * w + np.where(np.arange(count + 1) % 2, 0.5, -0.5) * rise
    for k in splinters:                                                # ... runs out far
        l_top[k] = (l_top[k - 1] + l_top[k + 1]) / 2 + (1 if k % 2 else -1) * rng.uniform(*SPLINTER) / half_l
    slant = rng.choice([-1, 1]) * rng.uniform(*BREAK_SLANT) / half_l
    top, bottom = np.c_[l_top, w], np.c_[l_top + slant, w]
    m = len(w)
    first = Piece(f'{name}-half-1', board,
                  [(-1, -1), *top, (-1, 1)], [(-1, -1), *bottom, (-1, 1)],
                  [('face', 1, -1)] + [('break', at)] * (m - 1) + [('face', 1, 1), ('face', 0, -1)])
    second = Piece(f'{name}-half-2', board,
                   [(1, -1), (1, 1), *top[::-1]], [(1, -1), (1, 1), *bottom[::-1]],
                   [('face', 0, 1), ('face', 1, 1)] + [('break', at)] * (m - 1) + [('face', 1, -1)])
    return [first, second]


# --- the crate --------------------------------------------------------------------------------------------------

def build(glb: Path, albedo: Path, out: Path, seed: int) -> dict:
    doc, primitives = read_glb(glb)
    groups = [doc['materials'][p['material']].get('extras', {}).get('group') for p in primitives]
    if groups.count('WOOD') != 1 or groups.count('METAL') != 1:
        raise ValueError(f'{glb}: expected one WOOD and one METAL primitive')
    wood, iron = primitives[groups.index('WOOD')], primitives[groups.index('METAL')]
    from PIL import Image   # only here: the board geometry is tested without pillow (scripts/fracture)
    image = np.asarray(Image.open(albedo).convert('RGB'), dtype=np.float64) / 255
    lum = (image ** 2.2) @ np.array([.2126, .7152, .0722])
    size = np.array(lum.shape[::-1])

    def island_lum(uv):
        lo = np.floor(uv.min(0) * size).astype(int) + 1
        hi = np.maximum(np.ceil(uv.max(0) * size).astype(int) - 1, lo + 1)
        return float(lum[lo[1]:hi[1], lo[0]:hi[0]].mean())

    boards, core = [], None
    for part in components(wood['position'], wood['triangles']):
        triangles = wood['triangles'][part]
        extent = np.ptp(wood['position'][triangles].reshape(-1, 3), axis=0)
        if extent.min() > 0.2:      # the dark core: inside the crate it stays empty
            if core is not None:
                raise ValueError('two core boxes')
            core = part
            continue
        boards.append(Board(wood['position'], wood['uv'], triangles, island_lum))
    if core is None:
        raise ValueError(f'{glb}: no core box')
    crate_centre = (wood['position'].min(0) + wood['position'].max(0)) / 2
    named: dict[str, list[tuple[float, Board]]] = {}
    for board in boards:
        role, key = board_name(board, crate_centre)
        named.setdefault(role, []).append((key, board))
    ordered = []
    for role in sorted(named):
        items = sorted(named[role], key=lambda item: item[0])
        for index, (_, board) in enumerate(items):
            ordered.append((f'{role}-{index + 1}' if len(items) > 1 else role, board))

    nails = []
    for part in components(iron['position'], iron['triangles']):
        triangles = iron['triangles'][part]
        centre = iron['position'][triangles].reshape(-1, 3).mean(0)
        outside = [np.linalg.norm((np.clip(b.local(centre), -1, 1) - b.local(centre)) * b.size / 2) for _, b in ordered]
        nails.append((int(np.argmin(outside)), triangles))

    rng = np.random.default_rng(seed)
    planks = [i for i, (name, b) in enumerate(ordered) if name.startswith(('plank', 'lid'))]
    wide = [i for i in planks if ordered[i][1].size[1] >= 0.14]
    n_split = int(rng.integers(3, 5))
    split_set = sorted(rng.choice(wide, n_split, replace=False).tolist())
    rest = [i for i in planks if i not in split_set]
    break_set = sorted(rng.choice(rest, int(rng.integers(1, 3)), replace=False).tolist())

    pieces = []
    for index, (name, board) in enumerate(ordered):
        mine = [tris for owner, tris in nails if owner == index]
        nails_lw = [tuple(board.local(iron['position'][t].reshape(-1, 3).mean(0))[:2]) for t in mine]
        if index in split_set:
            strips = 3 if board.size[1] >= 0.18 and rng.uniform() < 0.5 else 2
            parts = split(name, board, rng, strips, nails_lw)
        elif index in break_set:
            parts = broken(name, board, rng, nails_lw)
        else:
            parts = whole(name, board)
        for tris in mine:
            lw = board.local(iron['position'][tris].reshape(-1, 3).mean(0))[:2]
            owner = next((p for p in parts if p.contains(lw)), parts[0])
            owner.nails.append(tris)
        pieces.extend(parts)
    if len(pieces) > 64:
        raise ValueError(f'{len(pieces)} pieces exceed 64')

    nodes = []
    wood_index, iron_index = 0, 1
    for piece in pieces:
        tris = piece.triangles()
        position = np.vstack([p for p, _ in tris])
        uv = np.vstack([u for _, u in tris])
        normal = np.repeat([np.cross(p[1] - p[0], p[2] - p[0]) for p, _ in tris], 3, axis=0)
        normal /= np.linalg.norm(normal, axis=1, keepdims=True)
        prims = [(wood_index, position, normal, uv)]
        if piece.nails:
            ids = np.vstack(piece.nails).reshape(-1)
            prims.append((iron_index, iron['position'][ids], iron['normal'][ids], iron['uv'][ids]))
        nodes.append((piece.name, prims))
    materials = [doc['materials'][wood['material']], doc['materials'][iron['material']]]
    write_glb(out, materials, nodes)
    return dict(pieces=len(pieces), boards=len(boards), split=[ordered[i][0] for i in split_set],
                broken=[ordered[i][0] for i in break_set], nails=len(nails))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('glb', type=Path, help='the baked crate (atlas/model.glb)')
    parser.add_argument('albedo', type=Path, help='its wood atlas albedo.png (dark islands)')
    parser.add_argument('out', type=Path)
    parser.add_argument('--seed', type=int, default=7)
    args = parser.parse_args()
    print(json.dumps(build(args.glb, args.albedo, args.out, args.seed)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
