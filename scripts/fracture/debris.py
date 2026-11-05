"""Version 3 `.fracture` from a `model.debris.glb`: the pieces are the parts the thing is made of.

The debris GLB (convention in reports/content/2026-11-wood-fracture/README.md) holds one glTF node per
piece. The engine's converter (fracture-gltf-extract --nodes) gives each node's triangles in Gothic
centimetres with the material group of each triangle. A piece is one or more closed two-manifold
details plus open trims (nail heads, rivets) that only draw. Its physics is up to 16 convex hulls:

- every closed detail is cut by planes until each hull exceeds its part of the detail by at most 10%
  of that part's volume (a curved stave falls into a few lengths, a hoop into arcs);
- every hull is shrunk inward by 1.5 mm, so pieces that touch face to face spawn apart;
- hulls of different pieces may still overlap by at most 2 mm, measured as the diameter of the
  largest ball inside their intersection (linprog); a hull that overlaps more is cut further.

The drawing is the node's triangles as they are, with their UV and normals. Requires numpy and scipy.
"""
from __future__ import annotations

import io
from pathlib import Path
import zlib

import numpy as np
from scipy.optimize import linprog
from scipy.spatial import ConvexHull, HalfspaceIntersection, QhullError

EXCESS = 0.10        # hull volume beyond its part of the detail, share of that part
SHRINK = 0.15        # cm, every hull inward
OVERLAP = 0.2        # cm, largest ball diameter inside two hulls of different pieces
THICK = 0.1          # cm, the least half-thickness a cut hull keeps after shrinking
MAX_HULLS = 16
MAX_POINTS = 256
MAX_PIECES = 64
MAX_TRIANGLES = 20000
GROUPS = ('WOOD', 'METAL', 'STONE', 'CLAY', 'EARTH', 'WATER', 'SNOW', 'UNDEF')


def number(value):
    return "0" if abs(value) < 1e-12 else format(float(value), ".12g")


def read_nodes(path: Path):
    """[(name, vertices (T, 3, 8), textures, groups)] of fracture-gltf-extract --nodes output."""
    nodes = []
    lines = iter(path.read_text().splitlines())
    for line in lines:
        words = line.split()
        if words[0] == 'node' and len(words) == 2:
            nodes.append((words[1], [], [], []))
            continue
        if words[0] != 'tri' or len(words) != 3 or not nodes:
            raise ValueError(f'{path}: expected node or tri <texture> <group>')
        nodes[-1][1].append([[float(x) for x in next(lines).split()] for _ in range(3)])
        nodes[-1][2].append(words[1])
        nodes[-1][3].append(words[2])
    result = []
    for name, triangles, textures, groups in nodes:
        vertices = np.asarray(triangles, dtype=np.float64)
        if vertices.ndim != 3 or vertices.shape[1:] != (3, 8) or not np.isfinite(vertices).all():
            raise ValueError(f'{path}: {name}: malformed vertices')
        # The converter winds as Gothic draw meshes, clockwise around the outward normal; the cache winds
        # outward counterclockwise (ObjVisual::FractureView swaps back), as versions 1 and 2.
        result.append((name, vertices[:, [0, 2, 1]], textures, groups))
    return result


# --- closed meshes ---------------------------------------------------------------------------------------------

def details(positions):
    """Split triangles (T, 3, 3) into closed, consistently wound parts [(V, F)] and the open rest (indices)."""
    welded, inverse = np.unique(positions.reshape(-1, 3), axis=0, return_inverse=True)
    faces = inverse.reshape(-1, 3)
    parent = list(range(len(welded)))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for a, b, c in faces:
        for k in (b, c):
            x, y = root(int(a)), root(int(k))
            parent[max(x, y)] = min(x, y)
    groups = {}
    for index, face in enumerate(faces):
        groups.setdefault(root(int(face[0])), []).append(index)
    closed, rest = [], []
    for members in groups.values():
        part = faces[members]
        directed = {}
        for a, b, c in part:
            for edge in ((a, b), (b, c), (c, a)):
                directed[edge] = directed.get(edge, 0) + 1
        if all(n == 1 and directed.get(edge[::-1]) == 1 for edge, n in directed.items()):
            used, local = np.unique(part, return_inverse=True)
            closed.append((welded[used], local.reshape(-1, 3), members))
        else:
            rest.extend(members)
    return closed, sorted(rest)


def volume(V, F):
    """Signed volume and centroid of a closed mesh (positive when wound outward)."""
    a, b, c = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
    tetra = np.einsum('ij,ij->i', a, np.cross(b, c)) / 6
    total = tetra.sum()
    centroid = (tetra[:, None] * (a + b + c) / 4).sum(0) / total if abs(total) > 1e-12 else a.mean(0)
    return float(total), centroid


def earclip(points):
    """Triangles of a simple counter-clockwise 2D polygon; collinear runs end as zero-area triangles."""
    index = list(range(len(points)))
    scale = max(np.ptp(points, axis=0).max(), 1e-9)
    eps = 1e-12 * scale * scale
    triangles = []

    def cross(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])

    while len(index) > 3:
        best = None
        for k in range(len(index)):
            a, b, c = index[k - 1], index[k], index[(k + 1) % len(index)]
            area = cross(points[a], points[b], points[c])
            if area <= eps:
                continue
            if any(cross(points[a], points[b], points[j]) >= -eps and cross(points[b], points[c], points[j]) >= -eps
                   and cross(points[c], points[a], points[j]) >= -eps
                   for j in index if j not in (a, b, c) and not (np.array_equal(points[j], points[a]) or
                                                                 np.array_equal(points[j], points[c]))):
                continue
            best = k
            break
        if best is None:   # only collinear vertices left as candidates: clip the flattest
            areas = [abs(cross(points[index[k - 1]], points[index[k]], points[index[(k + 1) % len(index)]]))
                     for k in range(len(index))]
            best = int(np.argmin(areas))
            if areas[best] > eps:
                return None
        k = best
        triangles.append((index[k - 1], index[k], index[(k + 1) % len(index)]))
        index.pop(k)
    triangles.append(tuple(index))
    return triangles


def cut_mesh(V, F, normal, offset):
    """The closed mesh below and above the plane normal . x = offset, each capped; None when the plane
    misses, touches a vertex or leaves a cap with holes."""
    s = V @ normal - offset
    scale = np.ptp(V, axis=0).max()
    if np.abs(s).min() < 1e-7 * scale or (s > 0).all() or (s < 0).all():
        return None
    above = s > 0
    points = [*V]
    made = {}

    def cross_point(a, b):
        a, b = min(a, b), max(a, b)
        if (a, b) not in made:
            made[a, b] = len(points)
            points.append(V[a] + (V[b] - V[a]) * (s[a] / (s[a] - s[b])))
        return made[a, b]

    sides = {False: [], True: []}
    following = {}
    for tri in F:
        flags = above[tri]
        if flags.all() or not flags.any():
            sides[bool(flags[0])].append(tuple(tri))
            continue
        for r in range(3):
            a, b, c = np.roll(tri, -r)
            if above[a] != above[b] and above[b] == above[c]:
                break
        ab, ac = cross_point(a, b), cross_point(a, c)
        lone = bool(above[a])
        sides[lone].append((a, ab, ac))
        sides[not lone].extend(((ab, b, c), (ab, c, ac)))
        start, end = (ab, ac) if lone else (ac, ab)   # the cut edge as the upper side's surface runs it
        if start in following:
            return None
        following[start] = end
    points = np.asarray(points)
    # the lower cap (outward +normal) runs the upper surface's cut edges; the upper cap runs them back
    helper = np.eye(3)[int(np.argmin(np.abs(normal)))]
    e1 = np.cross(helper, normal)
    e1 /= np.linalg.norm(e1)
    e2 = np.cross(normal, e1)
    cap = []
    seen = set()
    for first in sorted(following):
        if first in seen:
            continue
        loop = [first]
        seen.add(first)
        while following.get(loop[-1]) != first:
            nxt = following.get(loop[-1])
            if nxt is None or nxt in seen:
                return None
            loop.append(nxt)
            seen.add(nxt)
        flat = np.c_[points[loop] @ e1, points[loop] @ e2]
        area = 0.5 * np.sum(flat[:, 0] * np.roll(flat[:, 1], -1) - np.roll(flat[:, 0], -1) * flat[:, 1])
        if area <= 0:       # a hole: such a cut is not used
            return None
        triangles = earclip(flat)
        if triangles is None:
            return None
        cap.extend(tuple(loop[i] for i in t) for t in triangles)
    result = []
    for side, cap_faces in ((False, cap), (True, [t[::-1] for t in cap])):
        faces = np.asarray(sides[side] + cap_faces, dtype=np.int64)
        used, local = np.unique(faces, return_inverse=True)
        result.append((points[used], local.reshape(-1, 3)))
    return result


# --- hulls -----------------------------------------------------------------------------------------------------

class Cell:
    """A closed part of one detail and its convex hull."""
    made = 0

    def __init__(self, piece, V, F):
        Cell.made += 1
        self.key = Cell.made
        self.piece, self.V, self.F = piece, V, F
        self.volume, _ = volume(V, F)
        hull = ConvexHull(V)
        self.hull_volume = hull.volume
        self.planes = np.unique(np.round(hull.equations, 12), axis=0)
        self.lower, self.upper = V.min(0), V.max(0)

    @property
    def excess(self):
        return self.hull_volume / max(self.volume, 1e-12) - 1


def chebyshev(planes, shrink=0.0):
    """Centre and radius of the largest ball inside {x: n . x + d + shrink <= 0}; radius < 0 when empty."""
    normals, offsets = planes[:, :3], planes[:, 3] + shrink
    lengths = np.linalg.norm(normals, axis=1)
    result = linprog([0, 0, 0, -1], A_ub=np.c_[normals, lengths], b_ub=-offsets,
                     bounds=[(None, None)] * 4, method='highs')
    if result.status != 0:
        raise ValueError(f'linprog failed: {result.message}')
    return result.x[:3], float(result.x[3])


def overlap(a_planes, b_planes):
    """Diameter of the largest ball inside both shrunk hulls; negative when they are apart."""
    return 2 * chebyshev(np.vstack((a_planes, b_planes)), SHRINK)[1]


def near(a, b):
    return bool(np.all(a.lower - SHRINK <= b.upper + SHRINK) and np.all(b.lower - SHRINK <= a.upper + SHRINK))


def candidates(cell, other=None):
    """Cut planes: across each principal axis, between consecutive vertices and at the octiles; the planes
    of the detail's own faces that run on into it (beside a notch or a tooth); and the faces of the hull
    of another piece it overlaps, whose outer side keeps clear of that hull."""
    centred = cell.V - cell.V.mean(0)
    _, _, axes = np.linalg.svd(centred, full_matrices=False)
    planes = []
    for axis in axes:
        axis = axis * (1 if axis[np.argmax(np.abs(axis))] > 0 else -1)
        values = np.unique(np.round(cell.V @ axis, 4))
        middles = (values[1:] + values[:-1]) / 2 if other is None else np.array([])
        if len(middles) > 32:
            middles = middles[np.linspace(0, len(middles) - 1, 32).round().astype(int)]
        low, high = values[0], values[-1]
        octiles = low + (high - low) * np.arange(1, 8) / 8
        for offset in np.unique(np.r_[middles, octiles]):
            planes.append((axis, float(offset)))
    a, b, c = (cell.V[cell.F[:, k]] for k in range(3))
    normals = np.cross(b - a, c - a)
    lengths = np.linalg.norm(normals, axis=1)
    seen = set()
    for normal, length, point in zip(normals, lengths, a):
        if length < 1e-9:
            continue
        normal = normal / length
        offset = float(normal @ point) - 1e-4
        key = (*np.round(normal, 5), round(offset, 3))
        if key in seen:
            continue
        seen.add(key)
        side = cell.V @ normal - offset
        if side.max() > 0.05 and side.min() < -0.05:     # the face's plane runs on through the detail
            planes.append((normal, offset))
    for plane in () if other is None else other.planes:
        normal = plane[:3] / np.linalg.norm(plane[:3])
        offset = -plane[3] / np.linalg.norm(plane[:3]) + 1e-4
        side = cell.V @ normal - offset
        if side.max() > 0.05 and side.min() < -0.05:
            planes.append((normal, offset))
    return planes


def split(cell, score, other=None):
    """The two children of the cut with the lowest score(children) that leaves no sliver, or None."""
    scored = []
    for axis, offset in candidates(cell, other):
        parts = cut_mesh(cell.V, cell.F, axis, offset)
        if parts is None:
            continue
        try:   # a cut may leave several disjoint bits on one side (the tips of two teeth): each its own cell
            children = [Cell(cell.piece, W, G) for V, F in parts for W, G, _ in details(V[F])[0]]
        except QhullError:
            continue
        if min(c.volume for c in children) > 1e-6 * cell.volume:
            scored.append((score(children), len(scored), children))
    scored.sort(key=lambda item: item[:2])
    for key, _, children in scored:
        # a hull must keep a core after shrinking
        if min(chebyshev(c.planes)[1] for c in children) >= SHRINK + THICK:
            return key, children
    return None


def shrunk_points(cell):
    centre, radius = chebyshev(cell.planes, SHRINK)
    if radius <= 1e-4:
        raise ValueError(f'hull thinner than {2 * SHRINK} cm')
    planes = cell.planes.copy()
    planes[:, 3] += SHRINK
    points = HalfspaceIntersection(planes, centre).intersections
    hull = ConvexHull(points)
    points = points[np.sort(hull.vertices)]
    order = np.lexsort(points.T[::-1])
    return points[order]


def hulls(pieces, names):
    """Cells per piece: detail cuts to <= EXCESS, then cuts until no two pieces overlap by > OVERLAP."""
    cells = []
    for index, closed in enumerate(pieces):
        for V, F in closed:
            work = [Cell(index, V, F)]
            while work:
                cell = work.pop()
                if cell.excess <= EXCESS:
                    cells.append(cell)
                    continue
                best = split(cell, lambda children: sum(c.hull_volume for c in children))
                if best is None:
                    raise ValueError(f'{names[index]}: a detail cannot be cut below {EXCESS:.0%} hull excess')
                work.extend(best[1][::-1])
        count = sum(c.piece == index for c in cells)
        if count > MAX_HULLS:
            raise ValueError(f'{names[index]}: {count} hulls exceed {MAX_HULLS}')

    depths = {}

    def depth(a, b):
        key = (min(a.key, b.key), max(a.key, b.key))
        if key not in depths:
            depths[key] = overlap(a.planes, b.planes)
        return depths[key]

    def worst():
        found = None
        for i, a in enumerate(cells):
            for j in range(i + 1, len(cells)):
                b = cells[j]
                if a.piece == b.piece or not near(a, b):
                    continue
                d = depth(a, b)
                if d > OVERLAP and (found is None or d > found[0]):
                    found = (d, i, j)
        return found

    def badness(cell, target):
        return max(overlap(cell.planes, target.planes) - OVERLAP, 0.0) * cell.hull_volume

    while (pair := worst()) is not None:
        d, i, j = pair
        options = []
        for mine, other in ((i, j), (j, i)):
            if sum(c.piece == cells[mine].piece for c in cells) >= MAX_HULLS:
                continue
            target = cells[other]
            before = badness(cells[mine], target)
            best = split(cells[mine], lambda children, t=target: (
                sum(badness(c, t) for c in children), sum(c.hull_volume for c in children)), target)
            if best is not None and best[0][0] < before * (1 - 1e-3):
                options.append(((best[0][0] / before, best[0][1]), mine, best[1]))
        if not options:
            a, b = cells[i].piece, cells[j].piece
            raise ValueError(f'{names[a]} and {names[b]} overlap by {10 * d:.1f} mm; '
                             f'cutting their hulls (at most {MAX_HULLS} a piece) does not separate them')
        options.sort(key=lambda option: option[0])
        _, mine, children = options[0]
        cells[mine:mine + 1] = children
    return cells


# --- version 3 -------------------------------------------------------------------------------------------------

def cut(nodes, glb: bytes):
    """Version 3 bytes of the debris nodes [(name, vertices, textures, groups)], bound to the model GLB."""
    if not 2 <= len(nodes) <= MAX_PIECES:
        raise ValueError(f'{len(nodes)} debris pieces outside [2, {MAX_PIECES}]')
    names = [name for name, *_ in nodes]
    pieces, meta = [], []
    for name, vertices, textures, groups in nodes:
        if not 1 <= len(vertices) <= MAX_TRIANGLES:
            raise ValueError(f'{name}: {len(vertices)} triangles outside [1, {MAX_TRIANGLES}]')
        if any(group not in GROUPS for group in groups):
            raise ValueError(f'{name}: unknown material group')
        closed, _ = details(vertices[:, :, :3])
        if not closed:
            raise ValueError(f'{name}: no closed detail')
        total, moment = 0.0, np.zeros(3)
        for V, F, _ in closed:
            v, c = volume(V, F)
            if v <= 0:
                raise ValueError(f'{name}: a closed detail is wound inward or flat')
            total += v
            moment += v * c
        a, b, c = vertices[:, 0, :3], vertices[:, 1, :3], vertices[:, 2, :3]
        areas = np.linalg.norm(np.cross(b - a, c - a), axis=1)
        by_group = {}
        for group, area in zip(groups, areas):
            by_group[group] = by_group.get(group, 0) + area
        group = max(sorted(by_group), key=by_group.get)
        pieces.append([(V, F) for V, F, _ in closed])
        meta.append((total, moment / total, group))
    cells = hulls(pieces, names)
    out = io.StringIO()
    out.write(f'GOTHIC_FRACTURE 3\nsource {len(glb)} {zlib.crc32(glb):08x}\npieces {len(nodes)}\n')
    for index, (name, vertices, textures, _) in enumerate(nodes):
        total, centre, group = meta[index]
        mine = [cell for cell in cells if cell.piece == index]
        out.write('piece ' + ' '.join(number(x) for x in (total, *centre)) + f' {group} {len(mine)} {len(vertices)}\n')
        for cell in mine:
            try:
                points = shrunk_points(cell)
            except (ValueError, QhullError) as error:
                raise ValueError(f'{name}: {error}') from None
            if len(points) > MAX_POINTS:
                raise ValueError(f'{name}: a hull of {len(points)} points exceeds {MAX_POINTS}')
            out.write(f'hull {len(points)}\n')
            for point in points:
                out.write(' '.join(number(x) for x in point) + '\n')
        for triangle, texture in zip(vertices, textures):
            out.write(f'tri {texture}\n')
            for vertex in triangle:
                out.write(' '.join(number(x) for x in vertex) + '\n')
    return out.getvalue().encode('ascii')


def validate(data: bytes, glb: bytes | None):
    """Check version 3 bytes as the game reads them: counts, the GLB binding, closed details, hulls and
    their overlaps. Returns dict(pieces, hulls, volume, hull_volume, largest (cm3), overlap (cm))."""
    lines = iter(data.decode('ascii').splitlines())
    if next(lines) != 'GOTHIC_FRACTURE 3':
        raise ValueError('expected version 3')
    source = next(lines).split()
    if (len(source) != 3 or source[0] != 'source' or glb is None or
            int(source[1]) != len(glb) or int(source[2], 16) != zlib.crc32(glb)):
        raise ValueError('fracture source does not match GLB size and CRC32')
    header = next(lines).split()
    count = int(header[1])
    if header[0] != 'pieces' or not 2 <= count <= MAX_PIECES:
        raise ValueError(f'invalid piece count {header}')
    hull_planes, volume_sum, hull_sum, largest = [], 0.0, 0.0, 0.0
    for index in range(count):
        words = next(lines).split()
        if len(words) != 8 or words[0] != 'piece' or words[5] not in GROUPS:
            raise ValueError(f'piece {index}: invalid header')
        declared, centre = float(words[1]), np.array([float(x) for x in words[2:5]])
        h, t = int(words[6]), int(words[7])
        if declared <= 0 or not 1 <= h <= MAX_HULLS or not 1 <= t <= MAX_TRIANGLES:
            raise ValueError(f'piece {index}: volume, hull or triangle count out of range')
        for _ in range(h):
            words = next(lines).split()
            if len(words) != 2 or words[0] != 'hull' or not 4 <= int(words[1]) <= MAX_POINTS:
                raise ValueError(f'piece {index}: invalid hull')
            points = np.array([[float(x) for x in next(lines).split()] for _ in range(int(words[1]))],
                              dtype=np.float32).astype(np.float64)
            hull = ConvexHull(points)
            if hull.volume <= 1e-6:
                raise ValueError(f'piece {index}: flat hull')
            hull_planes.append((index, np.unique(np.round(hull.equations, 12), axis=0), points.min(0), points.max(0)))
            hull_sum += hull.volume
        triangles = []
        for _ in range(t):
            if not next(lines).startswith('tri '):
                raise ValueError(f'piece {index}: missing triangle texture')
            vertex = np.array([[float(x) for x in next(lines).split()] for _ in range(3)], dtype=np.float32)
            if vertex.shape != (3, 8) or not np.isfinite(vertex).all():
                raise ValueError(f'piece {index}: invalid triangle')
            triangles.append(vertex.astype(np.float64))
        closed, _ = details(np.asarray(triangles)[:, :, :3])
        measured = [volume(V, F) for V, F, _ in closed]
        total = sum(v for v, _ in measured)
        if not closed or not np.isclose(total, declared, rtol=1e-4):
            raise ValueError(f'piece {index}: volume differs from its closed details')
        if np.linalg.norm(sum(v * c for v, c in measured) / total - centre) > 1e-3:
            raise ValueError(f'piece {index}: centre differs from its closed details')
        volume_sum += declared
        largest = max(largest, declared)
    if next(lines, None) is not None:
        raise ValueError('trailing data')
    worst = -np.inf
    for i, (a, a_planes, a_low, a_high) in enumerate(hull_planes):
        for b, b_planes, b_low, b_high in hull_planes[i + 1:]:
            if a == b or np.any(a_low > b_high) or np.any(b_low > a_high):
                continue
            worst = max(worst, 2 * chebyshev(np.vstack((a_planes, b_planes)))[1])
    if worst > OVERLAP + 1e-4:
        raise ValueError(f'hulls of different pieces overlap by {10 * worst:.2f} mm')
    return dict(pieces=count, hulls=len(hull_planes), volume=volume_sum, hull_volume=hull_sum,
                largest=largest, overlap=max(worst, 0.0))
