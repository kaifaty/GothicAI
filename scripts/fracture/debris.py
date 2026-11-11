"""Version 3 `.fracture` from a `model.debris.glb`: the pieces are the parts the thing is made of.

The debris GLB (convention in reports/content/2026-11-wood-fracture/README.md) holds one glTF node per
piece. The engine's converter (fracture-gltf-extract --nodes) gives each node's triangles in Gothic
centimetres with the material group of each triangle. A piece is one or more closed two-manifold
details plus open trims (nail heads) that only draw. Its physics is up to 16 convex hulls, an
approximation of its details rather than their copy (the shape rule, same README):

- no point of a hull's shape stands more than 8 mm outside its detail, and the shapes cover at least 85%
  of each detail's volume (70% of a ring's);
- a torn end's teeth and splinters beyond the line of its valleys only draw: the body is cut there by the
  plane through the valleys (the lower envelope of the tear) and its hulls are built from the body;
- a bent detail (a stave) falls into equal lengths until each length's hull keeps within 8 mm;
- a ring (a closed band, a hoop) falls into up to 16 arcs, cut at the corners of its polygon; each arc's
  prism lies on the ring's outer side and reaches at most 2 mm inside its inner surface (into the staves);
- a small closed detail (a rivet, a seam plate: under 2% of the piece's largest) joins the nearest hull;
- every hull is then shrunk inward by 1.5 mm, so pieces that touch face to face spawn apart; the shape
  rule is measured before this shrink (validate grows the written hulls back by it);
- hulls of different pieces may still overlap by at most 2 mm, measured as the diameter of the largest
  ball inside their intersection (linprog); a hull that overlaps more is cut further.

Where the rule cannot hold the cut fails with the piece's name and its numbers. The drawing is the node's
triangles as they are, with their UV and normals.

After the pieces comes the bond block (reports/physics/2026-11-destruction-bonds/README.md): two pieces that
touch (samples of one's closed details within 3 mm of the other's faces over 1 cm2 or more) are bonded by the
generator's names: a barrel's staves to their hoops (HOOP), head boards to the staves (GROOVE) and to each
other (DOWEL); a crate's details of different orientation (NAIL), never one wall's boards edge to edge; the
strips of one split board (ALONG) and the halves of one broken board or stave (ACROSS). Debris with other
names gets no block, as before bonds. Requires numpy and scipy.
"""
from __future__ import annotations

import io
import math
from pathlib import Path
import zlib

import numpy as np
from scipy.optimize import linprog
from scipy.spatial import ConvexHull, HalfspaceIntersection, QhullError

OUTSIDE = 0.8        # cm, the farthest a hull's shape stands outside its detail
COVER = 0.85         # the least share of a detail's volume its piece's hull shapes cover
RING_COVER = 0.70    # the same for a ring (a hoop, a thin bent band): its hulls stand at most 8 mm out and 2 mm
                     # into the staves, and 16 prisms cover no more of a band 8 mm thick (0.73-0.75 of the barrel's)
INWARD = 0.2         # cm, the deepest a ring's prism reaches inside the ring's inner surface
SMALL = 0.02         # a closed detail under this share of its piece's largest joins the nearest hull
SHARP = math.cos(math.radians(15))   # a concave edge sharper than 15 degrees marks a tear ...
END = 0.3            # ... within this share of the detail's length from one of its ends
SLOT = 2 * OUTSIDE   # cm, a notch of a tear narrower than this across the detail stays in its body:
                     # the hull spans it within OUTSIDE
BENT = 0.02          # a detail whose convex hull exceeds it by more is bent (report only)
SAMPLE = 0.8         # cm between samples on a hull's faces; the 16 farthest are refined 8 times finer
SHRINK = 0.15        # cm, every hull inward
OVERLAP = 0.2        # cm, largest ball diameter inside two hulls of different pieces
THICK = 0.05         # cm, the least radius a cut hull keeps after shrinking
NUDGE = 0.01         # cm, a cut along a face runs this far outside it, clear of its vertices: inside, the
                     # cut would leave a skin of the face on the far side, joined at the concave edge
CONTACT = 0.3        # cm, two pieces closer than this touch (a stave joint is 0.5 mm, a head board gap 1 mm)
MIN_CONTACT = 1.0    # cm2, the least touching area of a bond: a corner grazing a neighbour is none
BOND_STEP = 0.5      # cm between the samples that find the touching area
BOND_KINDS = ('NAIL', 'HOOP', 'GROOVE', 'DOWEL', 'ALONG', 'ACROSS')
MAX_BONDS = 1024
MAX_HULLS = 16
MAX_POINTS = 256
MAX_PIECES = 64
MAX_TRIANGLES = 20000
GROUPS = ('WOOD', 'METAL', 'STONE', 'CLAY', 'EARTH', 'WATER', 'SNOW', 'UNDEF')
CLASSES = ('board', 'bent', 'torn', 'ring')
SKEW = np.array([0.5773502691896258, 0.5773502691896258, 0.5773502691896258]) + [0.0123, -0.0311, 0.0077]
SKEW /= np.linalg.norm(SKEW)   # a ray direction off every axis and diagonal of the generators' meshes


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
    points = np.asarray(points, dtype=np.float64)
    index = list(range(len(points)))
    scale = max(np.ptp(points, axis=0).max(), 1e-9)
    eps = 1e-12 * scale * scale
    triangles = []

    def cross(a, b, c):
        return (b[..., 0] - a[..., 0]) * (c[..., 1] - a[..., 1]) - (b[..., 1] - a[..., 1]) * (c[..., 0] - a[..., 0])

    while len(index) > 3:
        ring = np.asarray(index)
        a_all, b_all, c_all = points[np.roll(ring, 1)], points[ring], points[np.roll(ring, -1)]
        areas = cross(a_all, b_all, c_all)
        best = None
        for k in np.flatnonzero(areas > eps):
            a, b, c = a_all[k], b_all[k], c_all[k]
            mask = np.ones(len(ring), dtype=bool)
            mask[[k - 1, k, (k + 1) % len(ring)]] = False
            rest = b_all[mask]
            rest = rest[~(np.all(rest == a, axis=1) | np.all(rest == c, axis=1))]
            if not np.any((cross(a, b, rest) >= -eps) & (cross(b, c, rest) >= -eps) & (cross(c, a, rest) >= -eps)):
                best = int(k)
                break
        if best is None:   # only collinear vertices left as candidates: clip the flattest
            best = int(np.argmin(np.abs(areas)))
            if abs(areas[best]) > eps:
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
    if np.abs(s).min() < NUDGE / 10 or (s > 0).all() or (s < 0).all():
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


# --- the shape rule --------------------------------------------------------------------------------------------

def triangle_distance(points, tris):
    """Distance of each point (N, 3) to the nearest triangle (M, 3, 3) and that triangle's index: the distance to
    its plane where the point projects inside it, else to the nearest of its edges (dot products as matmuls)."""
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]
    normal = np.cross(b - a, c - a)
    size = np.linalg.norm(normal, axis=1)
    index = np.flatnonzero(size > 1e-10)                   # earclip's collinear runs draw nothing
    a, b, c, normal = a[index], b[index], c[index], normal[index] / size[index, None]
    sides = [(a, b), (b, c), (c, a)]
    inward = [np.cross(normal, q - p) for p, q in sides]  # in the plane, toward the triangle
    best = np.full(len(points), np.inf)
    which = np.zeros(len(points), dtype=np.int64)
    step = max(1, 2000000 // max(len(a), 1))
    for start in range(0, len(points), step):
        P = points[start:start + step]
        square = (P * P).sum(1)[:, None]
        plane = np.abs(P @ normal.T - (a * normal).sum(1))
        within = np.ones(plane.shape, dtype=bool)
        edge = np.full(plane.shape, np.inf)
        for (p, q), m in zip(sides, inward):
            within &= P @ m.T - (p * m).sum(1) >= 0
            d = q - p
            length = np.maximum((d * d).sum(1), 1e-24)
            along = P @ d.T - (p * d).sum(1)
            t = np.clip(along / length, 0, 1)
            to_p = square - 2 * P @ p.T + (p * p).sum(1)
            edge = np.minimum(edge, to_p - 2 * t * along + t * t * length)
        dist = np.where(within, plane, np.sqrt(np.maximum(edge, 0)))
        k = np.argmin(dist, axis=1)
        best[start:start + step] = dist[np.arange(len(k)), k]
        which[start:start + step] = index[k]
    return best, which


def crossings(origins, direction, tris):
    """Parameters t > 0 where each ray origin + t direction crosses the triangles: (N, M), inf where it misses
    (Moller-Trumbore, its dot products as matmuls)."""
    a = tris[:, 0]
    e1, e2 = tris[:, 1] - a, tris[:, 2] - a
    h = np.cross(direction, e2)
    det = np.einsum('ij,ij->i', e1, h)
    good = np.abs(det) > 1e-12
    inv = np.where(good, 1 / np.where(good, det, 1), 0)
    g, k = np.cross(e1, direction), np.cross(e1, e2)
    out = np.full((len(origins), len(tris)), np.inf)
    step = max(1, 2000000 // max(len(tris), 1))
    for start in range(0, len(origins), step):
        P = origins[start:start + step]
        u = (P @ h.T - (a * h).sum(1)) * inv
        v = (P @ g.T - (a * g).sum(1)) * inv
        t = (P @ k.T - (a * k).sum(1)) * inv
        hit = good & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-9)
        out[start:start + step] = np.where(hit, t, np.inf)
    return out


def inside(points, solids):
    """Whether each point lies inside any of the closed surfaces (each (M, 3, 3)): ray parity along SKEW."""
    result = np.zeros(len(points), dtype=bool)
    for tris in solids:
        result |= np.isfinite(crossings(points, SKEW, tris)).sum(1) % 2 == 1
    return result


def facet_samples(tris, step):
    """Points about `step` apart over each triangle (a grid in its plane, its edges and corners) and the index
    of their triangle."""
    points, owner = [], []
    for index, tri in enumerate(tris):
        sides = np.linalg.norm(tri[[1, 2, 0]] - tri, axis=1)
        k = int(np.argmax(sides))
        a, b, c = tri[k], tri[(k + 1) % 3], tri[(k + 2) % 3]
        base = sides[k]
        e1 = (b - a) / max(base, 1e-12)
        e2 = c - a - e1 * ((c - a) @ e1)
        height = np.linalg.norm(e2)
        e2 = e2 / max(height, 1e-12)
        x, y = np.meshgrid(np.arange(step / 2, base, step), np.arange(step / 2, height, step), indexing='ij')
        local = a + x.reshape(-1, 1) * e1 + y.reshape(-1, 1) * e2
        m = np.c_[b - a, c - a]
        uv = np.linalg.lstsq(m, (local - a).T, rcond=None)[0].T if len(local) else np.zeros((0, 2))
        grid = local[(uv >= 0).all(1) & (uv.sum(1) <= 1)]
        edges = [p + np.linspace(0, 1, int(math.ceil(np.linalg.norm(q - p) / step)) + 1)[:, None] * (q - p)
                 for p, q in ((a, b), (b, c), (c, a))]
        sample = np.vstack([grid, *edges])
        points.append(sample)
        owner.append(np.full(len(sample), index))
    return np.vstack(points), np.concatenate(owner)


def outside_samples(points, solids, solid=True):
    """Samples of the faces of the convex hull of `points`, their distance outside the closed `solids` and the
    index of the nearest triangle of the stacked solids. Samples run every SAMPLE cm; around the 16 farthest
    the grid is refined to SAMPLE / 8 (the distance changes by at most the step between samples). With
    `solid`, a sample inside a solid counts 0: a written hull's face may run inside its detail along a cut.
    The cutter's hulls hold their cells, so their faces never do."""
    hull = ConvexHull(points)
    faces = points[hull.simplices]
    tris = np.concatenate(solids)

    def measure(sample):
        dist, near = triangle_distance(sample, tris)
        out = np.flatnonzero(dist > 1e-3)
        if solid and len(out):
            dist[out[inside(sample[out], solids)]] = 0.0
        return dist, near

    sample, owner = facet_samples(faces, SAMPLE)
    dist, near = measure(sample)
    finer = []
    for k in np.argsort(-dist, kind='stable')[:16]:
        a, b, c = faces[owner[k]]
        e1 = (b - a) / np.linalg.norm(b - a)
        e2 = c - a - e1 * ((c - a) @ e1)
        e2 /= np.linalg.norm(e2)
        grid = np.linspace(-SAMPLE, SAMPLE, 17)
        local = (sample[k] + grid[:, None, None] * e1 + grid[None, :, None] * e2).reshape(-1, 3)
        uv = np.linalg.lstsq(np.c_[b - a, c - a], (local - a).T, rcond=None)[0].T   # kept inside the face
        finer.append(local[(uv >= -1e-9).all(1) & (uv.sum(1) <= 1 + 1e-9)])
    finer = np.vstack(finer)
    more, close = measure(finer)
    return np.vstack((sample, finer)), np.r_[dist, more], np.r_[near, close]


def outside(points, solids, solid=False):
    """The farthest a point of the convex hull of `points` stands outside the closed `solids`, in cm."""
    return float(outside_samples(points, solids, solid)[1].max())


def triangles(V, F):
    return V[F]


def euler(F):
    edges = np.unique(np.sort(np.r_[F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]], axis=1), axis=0)
    return len(np.unique(F)) - len(edges) + len(F)


def principal(V):
    centre = V.mean(0)
    return centre, np.linalg.svd(V - centre, full_matrices=False)[2]


def face_normals(V, F):
    a, b, c = (V[F[:, k]] for k in range(3))
    normals = np.cross(b - a, c - a)
    return normals, np.linalg.norm(normals, axis=1)


def tears(V, F):
    """[(normal, offset)] of each torn end's valley line: the plane under every point of the tear (the vertices
    of its end faces and of its sharp concave edges) that runs highest under the tear's middle, the lower
    envelope of the teeth; the teeth lie on its + side. Notches narrower than SLOT are closed first: they
    stay in the body, whose hull spans them."""
    _, (a1, a2, a3) = principal(V)
    normals, lengths = face_normals(V, F)
    unit = normals / np.maximum(lengths, 1e-12)[:, None]
    concave = concave_edges(_Mesh(V, F), normals, lengths)
    planes = []
    for sign in (1.0, -1.0):
        height = sign * (V @ a1)
        region = height >= height.max() - END * np.ptp(height)
        sharp = [edge for edge, (f, g) in concave if region[list(edge)].all() and unit[f] @ unit[g] < SHARP]
        if not sharp:
            continue
        ends = region[F].all(1) & (unit @ (sign * a1) > 0.5) & (lengths > 1e-9)
        tear = np.unique(np.r_[F[ends].ravel(), np.asarray(sharp).ravel()])
        u, w, h = V[tear] @ a2, V[tear] @ a3, height[tear]
        # a notch narrower than SLOT (where the other half's splinter was) does not lower the line: closing of
        # the tear's heights over the break (a disc, so outer and inner fibres torn at other heights stay
        # apart); the hull then spans the notch
        window = np.hypot(u[:, None] - u[None, :], w[:, None] - w[None, :]) <= SLOT / 2
        rim = np.where(window, h[None, :], -np.inf).max(1)
        h = np.maximum(h, np.where(window, rim[None, :], np.inf).min(1))
        result = linprog([-u.mean(), -w.mean(), -1.0], A_ub=np.c_[u, w, np.ones_like(u)], b_ub=h,
                         bounds=[(-1, 1), (-1, 1), (None, None)], method='highs')
        if result.status != 0:
            raise ValueError(f'valley line: {result.message}')
        alpha, beta, gamma = result.x
        normal = sign * a1 - alpha * a2 - beta * a3
        size = np.linalg.norm(normal)
        planes.append((normal / size, gamma / size))
    return planes


class _Mesh:
    def __init__(self, V, F):
        self.V, self.F = V, F


def closed_parts(V, F):
    return [(W, G) for W, G, _ in details(V[F])[0]]


def trim(V, F):
    """The body of a detail: its closed parts behind the valley line of each torn end."""
    parts = [(V, F)]
    for normal, offset in tears(V, F):
        kept = []
        for W, G in parts:
            side = W @ normal - offset
            if side.max() <= NUDGE:          # this part lies behind the line
                kept.append((W, G))
                continue
            for k in range(1, 9):           # just beyond the valleys, clear of every vertex
                cut = cut_mesh(W, G, normal, offset + k * NUDGE)
                if cut is not None:
                    break
            else:
                raise ValueError('the valley line of a tear does not cut the detail')
            kept.extend(closed_parts(*cut[0]))
        parts = kept
    return parts


def lengths(V, F, count):
    """The closed part in `count` equal lengths along its longest axis, or None when a cut does not serve."""
    _, (axis, _, _) = principal(V)
    values = V @ axis
    low, high = values.min(), values.max()
    parts, rest = [], (V, F)
    for j in range(1, count):
        offset = low + (high - low) * j / count
        for shift in (0, 1, -1, 2, -2, 3, -3):
            cut = cut_mesh(*rest, axis, offset + shift * NUDGE)
            if cut is not None:
                break
        else:
            return None
        below, above = closed_parts(*cut[0]), closed_parts(*cut[1])
        if len(above) != 1:
            return None
        parts.extend(below)
        rest = above[0]
    return parts + [rest]


def fit(V, F, budget, depth=0):
    """Closed parts of a body whose hulls stand at most OUTSIDE outside them, and their worst: the body itself,
    else the fewest equal lengths (up to 8) along its longest axis that serve, else halves across its axes in
    turn, each fitted again (twice at most)."""
    worst = outside(V, [triangles(V, F)])
    if worst <= OUTSIDE:
        return [(V, F)], worst
    for count in range(2, min(budget, 8) + 1):
        parts = lengths(V, F, count)
        if parts is None or len(parts) > budget:
            continue
        deviations = []
        for W, G in parts:
            deviations.append(outside(W, [triangles(W, G)]))
            if deviations[-1] > OUTSIDE:
                break
        else:
            return parts, max(deviations)
    if depth < 2 and budget >= 2:
        for normal, offset in middles(_Mesh(V, F)):
            cut = cut_mesh(V, F, normal, offset)
            halves = [] if cut is None else closed_parts(*cut[0]) + closed_parts(*cut[1])
            if not 2 <= len(halves) <= budget:
                continue
            result, deviation = [], 0.0
            try:
                for i, (W, G) in enumerate(halves):
                    more, d = fit(W, G, budget - len(result) - (len(halves) - i - 1), depth + 1)
                    result.extend(more)
                    deviation = max(deviation, d)
            except ValueError:
                continue
            return result, deviation
    raise ValueError(f'hulls stand {10 * worst:.1f} mm outside the detail (limit {10 * OUTSIDE:.0f} mm) '
                     f'within {budget} hulls')


def ring_frame(V):
    """Centre, axis and two radial directions of a ring."""
    centre, axes = principal(V)
    axis = axes[2]
    e1 = axes[0] - axis * (axes[0] @ axis)
    e1 /= np.linalg.norm(e1)
    return centre, axis, e1, np.cross(axis, e1)


def inner_faces(V, F, centre, axis):
    """Faces of (a part of) a ring that look toward its axis."""
    normals, size = face_normals(V, F)
    middle = V[F].mean(1) - centre
    radial = middle - np.outer(middle @ axis, axis)
    radial /= np.maximum(np.linalg.norm(radial, axis=1), 1e-12)[:, None]
    return (np.einsum('ij,ij->i', normals, radial) < -0.5 * size) & (size > 1e-9)


def arcs(V, F, count):
    """A ring (a closed band) in at most `count` arcs: cut at its corners (the polygon of the staves it lies on),
    evenly spread when it has more corners than arcs; each arc's inner side cut by the plane INWARD inside the
    farthest point of its inner surface along the arc's middle. The arcs lie on the ring's outer side."""
    centre, axis, e1, e2 = ring_frame(V)
    offset = V - centre
    angle = np.mod(np.arctan2(offset @ e2, offset @ e1), 2 * np.pi)
    radius = float(np.mean(np.hypot(offset @ e1, offset @ e2)))
    order = np.sort(angle)
    ends = np.flatnonzero(np.diff(np.r_[order, order[0] + 2 * np.pi]) > math.radians(0.5))
    corners = np.sort(order[ends])                           # the last angle of each cluster of vertices
    if len(corners) < 3:
        corners = np.arange(count) * 2 * np.pi / count
    if len(corners) > count:
        corners = corners[np.round(np.arange(count) * len(corners) / count).astype(int)]

    def at(t):
        return centre + radius * (math.cos(t) * e1 + math.sin(t) * e2)

    def radial(t):                                           # the plane through the axis at angle t
        normal = -math.sin(t) * e1 + math.cos(t) * e2
        return normal, float(normal @ centre)

    pieces = None
    for k in range(1, 9):                                    # just past the corners, clear of their vertices
        bounds = np.mod(corners + k * 2 * NUDGE / radius, 2 * np.pi)
        # the first cut runs through two bounds across the ring, so each half is at most about half a turn
        far = int(np.argmin(np.abs(np.mod(bounds - bounds[0], 2 * np.pi) - np.pi)))
        normal = np.cross(axis, at(bounds[far]) - at(bounds[0]))
        normal /= np.linalg.norm(normal)
        halves = cut_mesh(V, F, normal, float(normal @ at(bounds[0])))
        if halves is None:
            continue
        span = np.mod(bounds[far] - bounds[0], 2 * np.pi)
        pieces = []
        for mesh in halves:
            rel = np.mod(np.arctan2((mesh[0] - centre) @ e2, (mesh[0] - centre) @ e1) - bounds[0], 2 * np.pi)
            low, high = (0.0, span) if np.median(rel) < span else (span, 2 * np.pi)
            inner = sorted(r for r in np.mod(bounds - bounds[0], 2 * np.pi)
                           if low + math.radians(1) < r < high - math.radians(1))
            for r in inner:
                cut = cut_mesh(*mesh, *radial(bounds[0] + r))
                parts = None if cut is None else (closed_parts(*cut[0]), closed_parts(*cut[1]))
                if parts is None or len(parts[0]) != 1 or len(parts[1]) != 1:
                    pieces = None
                    break
                pieces.append(parts[0][0])
                mesh = parts[1][0]
            if pieces is None:
                break
            pieces.append(mesh)
        if pieces is not None:
            break
    if pieces is None:
        raise ValueError('a ring does not cut at its corners')
    result = []
    for W, G in pieces:
        faces = inner_faces(W, G, centre, axis)
        normals, _ = face_normals(W, G)
        normal = -normals[faces].sum(0)                      # outward, across the arc's inner faces
        normal /= np.linalg.norm(normal)
        level = float((W[np.unique(G[faces])] @ normal).max()) - INWARD
        if (W @ normal).min() >= level:                      # one facet of the polygon: the arc is convex
            result.append((W, G))
            continue
        for k in range(8):
            cut = cut_mesh(W, G, normal, level + k * NUDGE)
            if cut is not None:
                break
        else:
            raise ValueError('a ring arc does not cut along its inner side')
        result.extend(closed_parts(*cut[1]))
    return result


def kind(V, F):
    """The class of a closed detail: ring, torn, bent or board."""
    if euler(F) == 0:
        return 'ring'
    if tears(V, F):
        return 'torn'
    return 'bent' if ConvexHull(V).volume > (1 + BENT) * volume(V, F)[0] else 'board'


def coverage(tris, shapes, step=0.2):
    """Share of the closed surface's volume inside the union of the convex shapes (each planes (K, 4),
    n . x + d <= 0): along lines through it on a grid `step` apart, across its thinnest axis, exact on each line.
    A line that grazes an edge (odd crossings) is left out."""
    points = tris.reshape(-1, 3)
    centre, (e1, e2, axis) = principal(points)
    x, y, z = (points - centre) @ e1, (points - centre) @ e2, (points - centre) @ axis
    gx = np.arange(x.min() + 0.371 * step, x.max(), step)
    gy = np.arange(y.min() + 0.613 * step, y.max(), step)
    origins = (centre + (z.min() - 1) * axis + gx[:, None, None] * e1 + gy[None, :, None] * e2).reshape(-1, 3)
    total = covered = 0.0
    for start in range(0, len(origins), 4096):
        o = origins[start:start + 4096]
        t = np.sort(crossings(o, axis, tris), axis=1)
        count = np.isfinite(t).sum(1)
        keep = (count > 0) & (count % 2 == 0)
        o, t, count = o[keep], t[keep], count[keep]
        if not len(o):
            continue
        t = t[:, :count.max()]
        lows, highs = [], []
        for planes in shapes:
            n, d = planes[:, :3], planes[:, 3]
            num, den = o @ n.T + d, n @ axis
            with np.errstate(divide='ignore', invalid='ignore'):
                bound = -num / den
            low = np.where(den < -1e-12, bound, -np.inf).max(1)
            high = np.where(den > 1e-12, bound, np.inf).min(1)
            empty = ((np.abs(den) <= 1e-12) & (num > 0)).any(1) | (low >= high)
            lows.append(np.where(empty, 0, low))
            highs.append(np.where(empty, 0, high))
        lows, highs = np.array(lows).T, np.array(highs).T
        cuts = np.sort(np.c_[np.where(np.isfinite(t), t, 0), lows, highs, np.zeros(len(o))], axis=1)
        cuts = np.clip(cuts, 0, None)
        middle = (cuts[:, 1:] + cuts[:, :-1]) / 2
        span = np.diff(cuts, axis=1)
        in_detail = (t[:, None, :] < middle[:, :, None]).sum(2) % 2 == 1
        in_shape = ((lows[:, None, :] < middle[:, :, None]) & (middle[:, :, None] < highs[:, None, :])).any(2)
        total += float((span * in_detail).sum())
        covered += float((span * (in_detail & in_shape)).sum())
    return covered / total if total > 0 else 0.0


def nearest(detail, parts):
    """Index of the closed part [(V, F)] whose surface lies nearest the centre of a small detail."""
    centre = detail[0].mean(0)[None]
    return int(np.argmin([triangle_distance(centre, triangles(W, G))[0][0] for W, G in parts]))


# --- hulls -----------------------------------------------------------------------------------------------------

class Cell:
    """A closed part of one detail and its convex hull; `extras` are the small closed details [(V, F)] of the
    same piece that its hull takes in."""
    made = 0

    def __init__(self, piece, V, F, extras=()):
        Cell.made += 1
        self.key = Cell.made
        self.piece, self.V, self.F, self.extras = piece, V, F, list(extras)
        self.volume, _ = volume(V, F)
        self.points = np.vstack([V, *(W for W, _ in self.extras)]) if self.extras else V
        hull = ConvexHull(self.points)
        self.hull_volume = hull.volume
        self.planes = np.unique(np.round(hull.equations, 12), axis=0)
        self.lower, self.upper = self.points.min(0), self.points.max(0)

    def children(self, parts):
        """Cells of the closed parts this cell is cut into; each small detail goes to the nearest."""
        groups = [[] for _ in parts]
        for extra in self.extras:
            groups[nearest(extra, parts)].append(extra)
        return [Cell(self.piece, W, G, group) for (W, G), group in zip(parts, groups)]

    def solids(self):
        return [triangles(self.V, self.F), *(triangles(W, G) for W, G in self.extras)]


def chebyshev(planes, shrink=0.0):
    """Centre and radius of the largest ball inside {x: n . x + d + shrink <= 0}; radius < 0 when empty."""
    normals, offsets = planes[:, :3], planes[:, 3] + shrink
    lengths = np.linalg.norm(normals, axis=1)
    for method in ('highs', 'highs-ds', 'highs-ipm'):   # the dual simplex now and then gives up on a near tie
        result = linprog([0, 0, 0, -1], A_ub=np.c_[normals, lengths], b_ub=-offsets,
                         bounds=[(None, None)] * 4, method=method)
        if result.status == 0:
            return result.x[:3], float(result.x[3])
    raise ValueError(f'linprog failed: {result.message}')


def overlap(a_planes, b_planes):
    """Diameter of the largest ball inside both shrunk hulls; negative when they are apart."""
    return 2 * chebyshev(np.vstack((a_planes, b_planes)), SHRINK)[1]


def near(a, b):
    return bool(np.all(a.lower - SHRINK <= b.upper + SHRINK) and np.all(b.lower - SHRINK <= a.upper + SHRINK))


def middles(cell):
    """Planes across each principal axis, longest first, through the middle of the extent (between vertices).
    The middle of a chord halves an arc radially: a bent stave or a hoop falls into arcs, not wedges."""
    _, _, axes = np.linalg.svd(cell.V - cell.V.mean(0), full_matrices=False)
    planes = []
    for axis in axes:
        values = np.unique(np.round(cell.V @ axis, 4))
        if len(values) > 1:
            gap = int(np.clip(np.searchsorted(values, (values[0] + values[-1]) / 2), 1, len(values) - 1))
            planes.append((axis, float(values[gap - 1] + values[gap]) / 2))
    return planes


def candidates(cell, other):
    """Cut planes against another piece's hull: across each principal axis at the octiles (between vertices),
    the planes of the detail's own faces that run on into it (beside a notch or a tooth), and the faces of
    that hull, whose outer side keeps clear of it."""
    _, _, axes = np.linalg.svd(cell.V - cell.V.mean(0), full_matrices=False)
    planes = []
    for axis in axes:
        values = np.unique(np.round(cell.V @ axis, 4))
        if len(values) < 2:
            continue
        gaps = np.clip(np.searchsorted(values, values[0] + np.ptp(values) * np.arange(1, 8) / 8), 1, len(values) - 1)
        for offset in np.unique((values[gaps - 1] + values[gaps]) / 2):
            planes.append((axis, float(offset)))
    a, b, c = (cell.V[cell.F[:, k]] for k in range(3))
    normals = np.cross(b - a, c - a)
    lengths = np.linalg.norm(normals, axis=1)
    seen, faces = set(), []
    for normal, length, point in zip(normals, lengths, a):
        if length < 1e-9:
            continue
        normal = normal / length
        offset = float(normal @ point) + NUDGE
        key = (*np.round(normal, 5), round(offset, 3))
        if key in seen:
            continue
        seen.add(key)
        side = cell.V @ normal - offset
        if side.max() > 0.05 and side.min() < -0.05:     # the face's plane runs on through the detail
            faces.append((normal, offset))
    if len(faces) > 24:   # a smooth curved detail has many: an even sample of them
        faces = [faces[i] for i in np.linspace(0, len(faces) - 1, 24).round().astype(int)]
    planes.extend(faces)
    planes.extend(valley_planes(cell, normals, lengths))
    for plane in other.planes:
        normal = plane[:3] / np.linalg.norm(plane[:3])
        offset = -plane[3] / np.linalg.norm(plane[:3]) + NUDGE
        side = cell.V @ normal - offset
        if side.max() > 0.05 and side.min() < -0.05:
            planes.append((normal, offset))
    return planes


def concave_edges(cell, normals, lengths):
    """[((a, b), (face, face))] of the edges where the detail turns inward."""
    faces_of = {}
    for index, face in enumerate(cell.F):
        for a, b in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            faces_of.setdefault((min(a, b), max(a, b)), []).append(index)
    concave = []
    for (a, b), faces in sorted(faces_of.items()):
        if len(faces) != 2 or min(lengths[faces]) < 1e-9:
            continue
        other = next(v for v in cell.F[faces[1]] if v not in (a, b))
        if (cell.V[other] - cell.V[a]) @ normals[faces[0]] / lengths[faces[0]] > 1e-3:
            concave.append(((a, b), tuple(faces)))
    return concave


def notch_planes(cell):
    """The planes of the faces at each concave edge, run on through the detail: cutting along one takes that
    concave edge away, so such cuts end in convex cells."""
    a, b, c = (cell.V[cell.F[:, k]] for k in range(3))
    normals = np.cross(b - a, c - a)
    lengths = np.linalg.norm(normals, axis=1)
    planes, seen = [], set()
    for _, faces in concave_edges(cell, normals, lengths):
        for face in faces:
            normal = normals[face] / lengths[face]
            offset = float(normal @ a[face]) + NUDGE
            key = (*np.round(normal, 5), round(offset, 3))
            side = cell.V @ normal - offset
            if key not in seen and side.max() > 0.05 and side.min() < -0.05:
                seen.add(key)
                planes.append((normal, offset))
    return planes


def valley_planes(cell, normals, lengths, limit=16):
    """Planes through two concave edges of the detail at once: the line of the valleys between teeth,
    which bounds the body behind them."""
    concave = [edge for edge, _ in concave_edges(cell, normals, lengths)][:limit]
    planes, seen = [], set()
    for i, (a, b) in enumerate(concave):
        for c, d in concave[i + 1:]:
            points = cell.V[[a, b, c, d]]
            centre = points.mean(0)
            _, values, axes = np.linalg.svd(points - centre)
            if values[1] < 0.05 or values[2] > 0.01:   # collinear, or not one plane
                continue
            normal = axes[2] * (1 if axes[2][np.argmax(np.abs(axes[2]))] > 0 else -1)
            offset = float(normal @ centre) + NUDGE
            key = (*np.round(normal, 4), round(offset, 2))
            side = cell.V @ normal - offset
            if key not in seen and side.max() > 0.05 and side.min() < -0.05:
                seen.add(key)
                planes.append((normal, offset))
    return planes


def split(cell, planes, score=None):
    """The children of the plane with the lowest score(children) that leaves no sliver; without a score the
    first such plane. None when no plane serves."""
    scored = []
    for axis, offset in planes:
        parts = cut_mesh(cell.V, cell.F, axis, offset)
        if parts is None:
            continue
        try:   # a cut may leave several disjoint bits on one side (the tips of two teeth): each its own cell
            children = cell.children([(W, G) for V, F in parts for W, G, _ in details(V[F])[0]])
        except QhullError:
            continue
        if len(children) < 2 or abs(sum(c.volume for c in children) - cell.volume) > 1e-6 * cell.volume or \
                min(c.volume - 1e-3 * c.hull_volume for c in children) <= 0:
            continue   # a cap that did not close or folded
        if min(c.volume for c in children) > 1e-6 * cell.volume:
            scored.append((score(children) if score else 0, len(scored), children))
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
    """Cells per piece under the shape rule, then cuts until no two pieces overlap by more than OVERLAP."""
    cells = []
    for index, closed in enumerate(pieces):
        name = names[index]
        volumes = [volume(V, F)[0] for V, F in closed]
        small = [k for k, v in enumerate(volumes) if len(closed) > 1 and v < SMALL * max(volumes)]
        main = [k for k in range(len(closed)) if k not in small]
        made = []
        for order, k in enumerate(main):
            V, F = closed[k]
            room = MAX_HULLS - len(made) - (len(main) - order - 1)
            try:
                if euler(F) == 0:
                    parts = arcs(V, F, room)
                else:
                    parts, bodies = [], trim(V, F)
                    for j, (W, G) in enumerate(bodies):
                        parts.extend(fit(W, G, room - len(parts) - (len(bodies) - j - 1))[0])
            except ValueError as error:
                raise ValueError(f'{name}: {error}') from None
            share = sum(volume(W, G)[0] for W, G in parts) / volumes[k]
            if share < (least := RING_COVER if euler(F) == 0 else COVER):
                raise ValueError(f'{name}: hulls cover {share:.3f} of a detail (least {least})')
            made.extend(parts)
        if len(made) > MAX_HULLS:
            raise ValueError(f'{name}: {len(made)} hulls exceed {MAX_HULLS}')
        groups = [[] for _ in made]
        for k in small:
            groups[nearest(closed[k], made)].append(closed[k])
        for (W, G), group in zip(made, groups):
            cell = Cell(index, W, G, group)
            if (far := outside(cell.points, cell.solids())) > OUTSIDE:
                raise ValueError(f'{name}: a hull stands {10 * far:.1f} mm outside the detail '
                                 f'(limit {10 * OUTSIDE:.0f} mm)')
            cells.append(cell)

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
            best = split(cells[mine], candidates(cells[mine], target), lambda children, t=target: (
                sum(badness(c, t) for c in children), sum(c.hull_volume for c in children)))
            if best is not None and best[0][0] < before * (1 - 1e-3):
                options.append(((best[0][0] / before, best[0][1]), mine, best[1]))
        if not options:   # no cut lowers the overlap at once: take a concave edge away, the least bad way
            for mine, other in ((i, j), (j, i)):
                if sum(c.piece == cells[mine].piece for c in cells) >= MAX_HULLS:
                    continue
                target = cells[other]
                best = split(cells[mine], notch_planes(cells[mine]), lambda children, t=target: (
                    sum(badness(c, t) for c in children), sum(c.hull_volume for c in children)))
                if best is not None:
                    options.append(((best[0][0] / max(badness(cells[mine], target), 1e-12), best[0][1]), mine, best[1]))
        if not options:
            a, b = cells[i].piece, cells[j].piece
            raise ValueError(f'{names[a]} and {names[b]} overlap by {10 * d:.1f} mm; '
                             f'cutting their hulls (at most {MAX_HULLS} a piece) does not separate them')
        options.sort(key=lambda option: option[0])
        _, mine, children = options[0]
        cells[mine:mine + 1] = children
    for index, name in enumerate(names):
        if (count := sum(c.piece == index for c in cells)) > MAX_HULLS:
            raise ValueError(f'{name}: {count} hulls exceed {MAX_HULLS}')
    return cells


# --- bonds -----------------------------------------------------------------------------------------------------

def thickness_axis(V):
    """The direction of a detail's least extent: through the thickness of a board."""
    return principal(V)[1][2]


def contact(a, b):
    """Where two pieces touch: (area in cm2, centre, normal) of the samples of each piece's closed details within
    CONTACT of the other's, the normal that of the other's nearest faces (sign of the first); None if apart."""
    lo_a, hi_a = a[0].reshape(-1, 3).min(0), a[0].reshape(-1, 3).max(0)
    lo_b, hi_b = b[0].reshape(-1, 3).min(0), b[0].reshape(-1, 3).max(0)
    if np.any(lo_a > hi_b + CONTACT) or np.any(lo_b > hi_a + CONTACT):
        return None
    area, points, normals = 0.0, [], []
    for (own, own_samples), (other, _) in ((a, b), (b, a)):
        lo, hi = other.reshape(-1, 3).min(0) - CONTACT, other.reshape(-1, 3).max(0) + CONTACT
        near = own_samples[np.all((own_samples >= lo) & (own_samples <= hi), axis=1)]
        if not len(near):
            continue
        distance, which = triangle_distance(near, other)
        touching = distance <= CONTACT
        area = max(area, float(touching.sum()) * BOND_STEP * BOND_STEP)
        points.append(near[touching])
        tri = other[which[touching]]
        n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        normals.append(n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12))
    if area < MIN_CONTACT:
        return None
    points, normals = np.vstack(points), np.vstack(normals)
    normals *= np.where(normals @ normals[0] < 0, -1.0, 1.0)[:, None]
    normal = normals.sum(0)
    return area, points.mean(0), normal / max(np.linalg.norm(normal), 1e-12)


def stem(name):
    """The detail a piece was cut from: a split board's strip, a broken board's half, a stave's top."""
    for mark in ('-strip-', '-half-'):
        if mark in name:
            return name.split(mark)[0], mark.strip('-')
    if name.endswith('-top'):
        return name[:-4], 'top'
    return name, None


def bond_kind(rules, a, b, touch, axes, groups):
    """The KIND of a bond between pieces named a, b (README of reports/physics/2026-11-destruction-bonds), or
    None when the two only touch: staves side by side, boards edge to edge in one wall."""
    (base_a, part_a), (base_b, part_b) = stem(a), stem(b)
    if base_a == base_b:
        return 'ALONG' if 'strip' in (part_a, part_b) else 'ACROSS'
    if rules == 'barrel':
        metal = [group == 'METAL' for group in groups]
        if any(metal):
            return 'HOOP' if not all(metal) else None
        heads = [name.startswith(('lid-', 'bottom-')) for name in (a, b)]
        if all(heads):
            return 'DOWEL' if a.split('-')[0] == b.split('-')[0] else None
        return 'GROOVE' if any(heads) else None
    # crate: details of different orientation that touch are nailed; one wall's boards lie edge to edge
    if abs(axes[0] @ axes[1]) > 0.9 and abs(touch @ axes[0]) < 0.5:
        return None
    return 'NAIL'


def bonding(nodes, pieces, groups):
    """[(a, b, KIND, point)] of the debris, a < b, by contact and the generator's names: 'barrel' when it has
    staves, 'crate' when it has planks and posts, else none (each piece breaks off alone, as before bonds)."""
    names = [name for name, *_ in nodes]
    if any(name.startswith('stave-') for name in names):
        rules = 'barrel'
    elif any(name.startswith(('plank-', 'post-')) for name in names):
        rules = 'crate'
    else:
        return []
    shapes = []
    for closed in pieces:
        tris = np.vstack([triangles(V, F) for V, F in closed])
        samples, _ = facet_samples(tris, BOND_STEP)
        shapes.append((tris, samples))
    axes = [thickness_axis(max(closed, key=lambda d: volume(*d)[0])[0]) for closed in pieces]
    result = []
    for i in range(len(nodes)):
        for j in range(i + 1, len(nodes)):
            touch = contact(shapes[i], shapes[j])
            if touch is None:
                continue
            kind_ = bond_kind(rules, names[i], names[j], touch[2], (axes[i], axes[j]), (groups[i], groups[j]))
            if kind_ is not None:
                result.append((i, j, kind_, touch[1]))
    if len(result) > MAX_BONDS:
        raise ValueError(f'{len(result)} bonds exceed {MAX_BONDS}')
    return result


def islands(count, bonds):
    """Connected groups of pieces over the bonds, each a sorted list of piece indices."""
    parent = list(range(count))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for a, b, *_ in bonds:
        parent[max(root(a), root(b))] = min(root(a), root(b))
    groups = {}
    for i in range(count):
        groups.setdefault(root(i), []).append(i)
    return sorted(groups.values())




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
    bonds = bonding(nodes, pieces, [group for _, _, group in meta])
    if bonds:
        out.write(f'bonds {len(bonds)}\n')
        for a, b, kind_, point in bonds:
            out.write(f'bond {a} {b} {kind_} ' + ' '.join(number(x) for x in point) + '\n')
    return out.getvalue().encode('ascii')


def grown(points):
    """The planes (K, 4) of the shape of a written hull: its faces moved back out by SHRINK. A face under
    0.01 mm2 is left out: at the thin ends of a hoop's arc the written float32 points give qhull slivers along
    sharp edges, whose planes grown back would cut a bevel off the shape. Where a face vanished in the shrink
    the grown shape is larger than the cut's."""
    hull = ConvexHull(points)
    corners = points[hull.simplices]
    area = np.linalg.norm(np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]), axis=1) / 2
    planes, which = np.unique(np.round(hull.equations, 12), axis=0, return_inverse=True)
    planes = planes[np.bincount(which.ravel(), area, len(planes)) >= 1e-4]
    planes[:, 3] -= SHRINK
    return planes


def validate(data: bytes, glb: bytes | None):
    """Check version 3 bytes as the game reads them: counts, the GLB binding, closed details, hulls, the shape
    rule and the overlaps of different pieces. Returns dict(pieces, hulls, volume, hull_volume, largest (cm3),
    overlap, outside (cm), cover and classes: per class of the pieces' largest details (board, bent, torn, ring)
    pieces, hulls, outside, overlap and for rings inward (cm); cover per class of each detail itself)."""
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
    classes = {}
    for index in range(count):
        words = next(lines).split()
        if len(words) != 8 or words[0] != 'piece' or words[5] not in GROUPS:
            raise ValueError(f'piece {index}: invalid header')
        declared, centre = float(words[1]), np.array([float(x) for x in words[2:5]])
        h, t = int(words[6]), int(words[7])
        if declared <= 0 or not 1 <= h <= MAX_HULLS or not 1 <= t <= MAX_TRIANGLES:
            raise ValueError(f'piece {index}: volume, hull or triangle count out of range')
        written = []
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
            written.append(points)
        triangles_ = []
        for _ in range(t):
            if not next(lines).startswith('tri '):
                raise ValueError(f'piece {index}: missing triangle texture')
            vertex = np.array([[float(x) for x in next(lines).split()] for _ in range(3)], dtype=np.float32)
            if vertex.shape != (3, 8) or not np.isfinite(vertex).all():
                raise ValueError(f'piece {index}: invalid triangle')
            triangles_.append(vertex.astype(np.float64))
        closed, _ = details(np.asarray(triangles_)[:, :, :3])
        measured = [volume(V, F) for V, F, _ in closed]
        total = sum(v for v, _ in measured)
        if not closed or not np.isclose(total, declared, rtol=1e-4):
            raise ValueError(f'piece {index}: volume differs from its closed details')
        if np.linalg.norm(sum(v * c for v, c in measured) / total - centre) > 1e-3:
            raise ValueError(f'piece {index}: centre differs from its closed details')
        volume_sum += declared
        largest = max(largest, declared)

        # the shape rule: how far out and in the written hulls stand, as physics takes them; the cover of the
        # shapes before the shrink, which only parts the pieces (it alone takes 14% of a 2.2 cm board)
        main = int(np.argmax([v for v, _ in measured]))
        V, F, _ = closed[main]
        cls = kind(V, F)
        solids = [triangles(W, G) for W, G, _ in closed]
        shapes = [grown(points) for points in written]
        stats = classes.setdefault(cls, dict(pieces=0, hulls=0, outside=0.0, cover=1.0, overlap=0.0))
        stats['pieces'] += 1
        stats['hulls'] += h
        inner = None
        if cls == 'ring':
            ring_centre, axis, _, _ = ring_frame(V)
            offsets = np.cumsum([0] + [len(s) for s in solids])
            inner = np.zeros(offsets[-1], dtype=bool)
            inner[offsets[main]:offsets[main + 1]] = inner_faces(V, F, ring_centre, axis)
            stats.setdefault('inward', 0.0)
        for k, points in enumerate(written):
            _, distance, nearest = outside_samples(points, solids)
            far = float(distance.max())
            if far > OUTSIDE + 1e-3:
                raise ValueError(f'piece {index}: hull {k} stands {10 * far:.1f} mm outside its details '
                                 f'(limit {10 * OUTSIDE:.0f} mm)')
            stats['outside'] = max(stats['outside'], far)
            if inner is not None:
                deep = float(distance[inner[nearest]].max(initial=0.0))
                if deep > INWARD + 1e-3:
                    raise ValueError(f'piece {index}: hull {k} reaches {10 * deep:.1f} mm inside the ring '
                                     f'(limit {10 * INWARD:.0f} mm)')
                stats['inward'] = max(stats['inward'], deep)
        for W, G, _ in closed:   # each detail by its own class: a hoop's rivets and seam plate are no ring
            own = kind(W, G)
            share = coverage(triangles(W, G), shapes)
            if share < (least := RING_COVER if own == 'ring' else COVER):
                raise ValueError(f'piece {index}: hulls cover {share:.3f} of a {own} detail (least {least})')
            row = classes.setdefault(own, dict(pieces=0, hulls=0, outside=0.0, cover=1.0, overlap=0.0))
            row['cover'] = min(row['cover'], share)
        hull_planes[-h:] = [(i, p, lo, hi, cls) for i, p, lo, hi in hull_planes[-h:]]
    bonds = []
    header = next(lines, None)
    if header is not None:
        words = header.split()
        if len(words) != 2 or words[0] != 'bonds' or not 1 <= int(words[1]) <= MAX_BONDS:
            raise ValueError(f'invalid bond count {words}')
        for _ in range(int(words[1])):
            words = next(lines, '').split()
            if len(words) != 7 or words[0] != 'bond' or words[3] not in BOND_KINDS:
                raise ValueError(f'invalid bond {words}')
            a, b = int(words[1]), int(words[2])
            point = np.array([float(x) for x in words[4:]])
            if not 0 <= a < b < count or not np.isfinite(point).all():
                raise ValueError(f'invalid bond {words}')
            if any((a, b) == (x, y) for x, y, _ in bonds):
                raise ValueError(f'bond {a} {b} repeated')
            bonds.append((a, b, words[3]))
    if next(lines, None) is not None:
        raise ValueError('trailing data')
    worst = -np.inf
    for i, (a, a_planes, a_low, a_high, a_cls) in enumerate(hull_planes):
        for b, b_planes, b_low, b_high, b_cls in hull_planes[i + 1:]:
            if a == b or np.any(a_low > b_high) or np.any(b_low > a_high):
                continue
            depth = 2 * chebyshev(np.vstack((a_planes, b_planes)))[1]
            worst = max(worst, depth)
            for cls in (a_cls, b_cls):
                classes[cls]['overlap'] = max(classes[cls]['overlap'], depth)
    if worst > OVERLAP + 1e-4:
        raise ValueError(f'hulls of different pieces overlap by {10 * worst:.2f} mm')
    kinds = {k: sum(kind_ == k for *_, kind_ in bonds) for k in BOND_KINDS if any(kind_ == k for *_, kind_ in bonds)}
    return dict(pieces=count, hulls=len(hull_planes), volume=volume_sum, hull_volume=hull_sum,
                largest=largest, overlap=max(worst, 0.0), bonds=kinds, islands=len(islands(count, bonds)),
                outside=max(s['outside'] for s in classes.values()),
                cover=min(s['cover'] for s in classes.values()),
                classes={cls: classes[cls] for cls in CLASSES if cls in classes})
