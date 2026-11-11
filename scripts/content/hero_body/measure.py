"""Tape-measure girths of an exported MPFB hero body, cm, and the ratios of the classical male canon.

    uv run --with numpy --with scipy python -I measure.py <run dir> [<run dir> ...]

Reads <run>/body.npz and <run>/braies.npz (mpfb_export.py; W frame, cm, y up). A girth is the convex-hull perimeter
of the mesh's cross-section by a plane, as a tape lies over the skin: across a limb the plane is perpendicular to
the bone's axis (the principal axis of the vertices it carries most) at a fraction of its length; the torso is cut
horizontally without the arms.

The canon is the classical (Grecian ideal) set for a fit, not built, man: neck ~ calf ~ relaxed upper arm + 2 cm,
thigh ~ 1.5 calf, shoulder girth ~ 1.6 waist (Adonis index), chest ~ 1.25 waist.
"""
import sys
import numpy as np
from scipy.spatial import ConvexHull

ARMS = ('UPPERARM', 'FOREARM', 'HAND', 'FINGER')


def load(run):
    parts = [np.load(f'{run}/{n}.npz') for n in ('body', 'braies')]
    groups = list(parts[0]['groups'])
    P, T, W, off = [], [], [], 0
    for p in parts:
        P.append(p['co']); W.append(p['weights']); T.append(p['tri_v'] + off); off += len(p['co'])
    return np.vstack(P), np.vstack(T), np.vstack(W), groups


def section(P, T, point, normal):
    """Points where the triangle edges cross the plane, and the triangle each comes from."""
    d = (P - point) @ normal
    pts, tri = [], []
    for a, b in ((0, 1), (1, 2), (2, 0)):
        da, db = d[T[:, a]], d[T[:, b]]
        m = (da * db) < 0
        t = da[m] / (da[m] - db[m])
        pts.append(P[T[m, a]] + (P[T[m, b]] - P[T[m, a]]) * t[:, None]); tri.append(np.where(m)[0])
    return np.vstack(pts), np.concatenate(tri)


def perimeter(pts2):
    if len(pts2) < 3:
        return float('nan')
    v = pts2[ConvexHull(pts2).vertices]
    return float(np.linalg.norm(v - np.roll(v, 1, 0), axis=1).sum())


def frame(n):
    e1 = np.cross(n, [1.0, 0, 0]) if abs(n[0]) < 0.9 else np.cross(n, [0, 0, 1.0])
    e1 /= np.linalg.norm(e1)
    return e1, np.cross(n, e1)


def limb(P, T, W, groups, name, frac, radius):
    own = W.argmax(1) == groups.index(name)
    Q = P[own]; c = Q.mean(0)
    axis = np.linalg.svd(Q - c, full_matrices=False)[2][0]
    if axis[1] < 0:
        axis = -axis                                  # up: the proximal end of a hanging limb, the top of the neck
    t = (Q - c) @ axis
    top, bottom = np.percentile(t, 98), np.percentile(t, 2)
    point = c + axis * (top + (bottom - top) * frac)  # frac from the proximal (upper) end
    pts, _ = section(P, T, point, axis)
    r = pts - point; r -= np.outer(r @ axis, axis)
    pts = pts[np.linalg.norm(r, axis=1) < radius]
    e1, e2 = frame(axis)
    return perimeter(np.c_[(pts - point) @ e1, (pts - point) @ e2]), float(point[1])


def torso(P, T, W, groups, y):
    pts, tri = section(P, T, np.array([0.0, y, 0.0]), np.array([0.0, 1.0, 0.0]))
    arm = np.isin(W.argmax(1), [i for i, g in enumerate(groups) if any(k in g for k in ARMS)])
    keep = ~arm[T[tri, 0]]
    return perimeter(pts[keep][:, [0, 2]])


def measure(run):
    P, T, W, groups = load(run)
    m = {}
    m['neck'], ny = limb(P, T, W, groups, 'BIP01 NECK', 0.5, 9.0)
    m['upper arm'], _ = limb(P, T, W, groups, 'BIP01 L UPPERARM', 0.55, 9.0)
    m['forearm'], _ = limb(P, T, W, groups, 'BIP01 L FOREARM', 0.2, 8.0)
    m['thigh'], _ = limb(P, T, W, groups, 'BIP01 L THIGH', 0.3, 12.0)
    m['calf'], _ = limb(P, T, W, groups, 'BIP01 L CALF', 0.3, 9.0)
    ys = np.arange(98, 150, 0.5)
    g = np.array([torso(P, T, W, groups, y) for y in ys])
    hi = ys > 125; lo = (ys > 100) & (ys < 122)
    chest_y = ys[hi][np.nanargmax(g[hi])]; waist_y = ys[lo][np.nanargmin(g[lo])]
    m['chest'] = torso(P, T, W, groups, chest_y)
    m['waist'] = torso(P, T, W, groups, waist_y)
    pts, _ = section(P, T, np.array([0.0, chest_y + 6, 0.0]), np.array([0.0, 1.0, 0.0]))
    m['shoulder girth'] = perimeter(pts[:, [0, 2]])
    band = (P[:, 1] > chest_y) & (P[:, 1] < chest_y + 14)
    m['shoulder breadth'] = float(np.ptp(P[band, 0]))
    m['at y (neck, chest, waist)'] = (round(ny, 1), float(chest_y), float(waist_y))
    return m


def ratios(m):
    return {
        'neck / upper arm (canon ~1.05)': m['neck'] / m['upper arm'],
        'neck / calf (canon ~1.0)': m['neck'] / m['calf'],
        'thigh / calf (canon ~1.5)': m['thigh'] / m['calf'],
        'shoulder girth / waist (canon ~1.6)': m['shoulder girth'] / m['waist'],
        'chest / waist (canon ~1.25)': m['chest'] / m['waist'],
    }


if __name__ == '__main__':
    for run in sys.argv[1:]:
        m = measure(run)
        print('==', run)
        for k, v in m.items():
            print(f'  {k:26s}', v if isinstance(v, tuple) else round(v, 1))
        for k, v in ratios(m).items():
            print(f'  {k:38s} {v:.2f}')
