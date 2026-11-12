"""Walkability check: compares the ground of a world before (A, the base) and after (B, the base with a content
folder on top) a change, offline.

    python3 scripts/content/world/world_walk_check.py NEWWORLD --content DIR [--base DIR]
        [--chunks 8_-4,8_-3 | --rect X0,Z0,X1,Z1] [--out DIR] [--path-tolerance 0.05] [--ground-tolerance 0.30]

Prints a Markdown report; with --out writes <WORLD>_walk.md, <WORLD>_walk.json and <WORLD>_walk_objects.json (the
objects whose ground moved, the input of the re-seat tool). Exits 1 when a limit is exceeded, 0 otherwise. Step Д2
of docs/landscape-plan.md; the rule (walkable ground within ±0.3 m, paths and quest places kept) is in
reports/engine/2026-11-12-world-detail/README.md, section 4.4.

B is read as world_census.py --content reads it: content chunks replace base chunks of the same name, the content
vobs.json replaces the base one, vobs.d layers are applied. The waynet is the base's waynet.json in both states.

What is measured, and how it follows the engine:
- Collision is the world mesh without water and without materials with disable_collision
  (OpenGothic/common/physics/dynamicworld.cpp, landMesh). Rays ignore back faces (collisionworld.cpp rayCast), so a
  ray straight down only hits up-facing triangles. Static objects' own collision (C_Object) is not modelled.
- Waypoint: WayHeights::adjust casts DynamicWorld::landRay straight down from the stored position over 200 m
  (worldHeight); the ground is the highest up-facing hit at or below the stored height. "Starts below the surface":
  B has an up-facing surface less than 2 m above the stored position, closer than any in A — the engine's ray then
  starts inside the ground and finds a lower floor or nothing.
- Edge: the ground profile of the segment sampled every 0.25 m (WayHeights::Profile spacing,
  min(25 cm, stepHeight / 2)), starting at the ground of the first waypoint and following the surface: at each sample
  the highest up-facing hit within the step band (previous height ± stepHeight + 1 cm, as WayHeights::profile
  searches), else the hit nearest to the previous height within 2 m, so a step over the limit is still measured (a
  sample with none is "no support"; the engine then throws "no continuous ground support"). Per edge: largest |B - A| height, steepest face (angle of the hit's normal
  from up, the test hit.n.y >= slopeCos) and highest step between neighbouring samples, in A and in B.
- Walkable ground: up-facing ground and rock triangles (census classes) above sea level not steeper than the slide
  angle, as census layers on a 1 m grid; per cell each layer's top in A is matched to the nearest layer top of B
  within 2 m. A layer with no partner within 2 m vanished (A) or appeared (B).
- Objects: every vob of B with its children (and the mesh additions of vobs.d layers). Ground in A: the highest
  up-facing hit at or below the object's height + 1 m, in A and in B.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import world_census as census  # noqa: E402
import world_classes as wc  # noqa: E402

# Walking limits of MoveAlgo (OpenGothic/common/game/movealgo.cpp). Both are guild values of the game scripts
# (C_GILVALUES.STEP_HEIGHT and SLIDE_ANGLE); every human guild copies GIL_HUMAN (gamescript.cpp), whose values are
# 60 cm and 50 degrees (reports/navigation/2026-11-04-nav-sim-research/data/rays/engine.json).
STEP_HEIGHT_M = 0.60        # MoveAlgo::stepHeight(), guild step_height; 50 cm when the guild has none
SLIDE_ANGLE_DEG = 50.0      # MoveAlgo::slideAngle() = cos(slide_angle): the steepest face a body stands on
STEP_BAND_M = STEP_HEIGHT_M + 0.01   # WayHeights::profile searches stepHeight + 1 cm around the previous floor
PROFILE_SPACING_M = 0.25    # WayHeights::Profile spacing = min(MaxProfileSpacing 25 cm, stepHeight / 2)
LAND_RAY_M = 200.0          # DynamicWorld::worldHeight, the reach of landRay
LAYER_GAP_M = census.LAYER_GAP_M
OBJECT_TOLERANCE_M = 0.05
OBJECT_REACH_M = 1.0        # an object's ground is searched from this far above its origin (sunk trees, rocks)
SLOPE_BINS = tuple(range(0, 91, 10))
REACH_EPS = 1e-4
LIST_ROWS = 40


# --- ground columns: every up-facing hit of a vertical line

class Columns:
    """Up-facing triangles of P (t, 3, 3), glTF metres, bucketed on an XZ grid for vertical-line queries."""

    def __init__(self, P: np.ndarray, bucket: float = 2.0):
        _, ny = census.triangle_areas(P)
        keep = ny > 0
        self.P, self.ny, self.bucket = P[keep], ny[keep], bucket
        if not len(self.P):
            self.keys, self.tris = np.zeros(0, np.int64), np.zeros(0, np.int64)
            return
        x, z = self.P[:, :, 0] / bucket, self.P[:, :, 2] / bucket
        i0, i1 = np.floor(x.min(1)).astype(np.int64), np.floor(x.max(1)).astype(np.int64)
        j0, j1 = np.floor(z.min(1)).astype(np.int64), np.floor(z.max(1)).astype(np.int64)
        nz = j1 - j0 + 1
        count = (i1 - i0 + 1) * nz
        tri = np.repeat(np.arange(len(self.P)), count)
        local = np.arange(count.sum()) - np.repeat(np.cumsum(count) - count, count)
        keys = self.key(i0[tri] + local // nz[tri], j0[tri] + local % nz[tri])
        order = np.argsort(keys, kind='stable')
        self.keys, self.tris = keys[order], tri[order]

    @staticmethod
    def key(i, j):
        return (np.asarray(i, np.int64) + (1 << 30)) * (1 << 31) + (np.asarray(j, np.int64) + (1 << 30))

    def hits(self, x: np.ndarray, z: np.ndarray, batch: int = 40_000):
        """All hits of the vertical lines at glTF (x, z): query index, height and normal y, sorted by query then
        height."""
        Q, H, N = [np.zeros(0, np.int64)], [np.zeros(0)], [np.zeros(0)]
        for b0 in range(0, len(x), batch):
            qx, qz = x[b0:b0 + batch], z[b0:b0 + batch]
            k = self.key(np.floor(qx / self.bucket), np.floor(qz / self.bucket))
            lo = np.searchsorted(self.keys, k, 'left')
            n = np.searchsorted(self.keys, k, 'right') - lo
            q = np.repeat(np.arange(len(qx)), n)
            t = self.tris[np.repeat(lo, n) + np.arange(n.sum()) - np.repeat(np.cumsum(n) - n, n)]
            p = self.P[t]
            px, pz = qx[q], qz[q]
            x0, x1, x2 = p[:, 0, 0], p[:, 1, 0], p[:, 2, 0]
            z0, z1, z2 = p[:, 0, 2], p[:, 1, 2], p[:, 2, 2]
            den = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2)
            ok = np.abs(den) > 1e-12
            den = np.where(ok, den, 1.0)
            l0 = ((z1 - z2) * (px - x2) + (x2 - x1) * (pz - z2)) / den
            l1 = ((z2 - z0) * (px - x2) + (x0 - x2) * (pz - z2)) / den
            l2 = 1 - l0 - l1
            e = -1e-9
            m = ok & (l0 >= e) & (l1 >= e) & (l2 >= e)
            h = l0 * p[:, 0, 1] + l1 * p[:, 1, 1] + l2 * p[:, 2, 1]
            Q.append(q[m] + b0)
            H.append(h[m])
            N.append(self.ny[t][m])
        q, h, ny = np.concatenate(Q), np.concatenate(H), np.concatenate(N)
        o = np.lexsort((ny, h, q))
        return q[o], h[o], ny[o]

    def column(self, x: np.ndarray, z: np.ndarray) -> Column:
        q, h, ny = self.hits(x, z)
        start = np.searchsorted(q, np.arange(len(x) + 1))
        return Column(start, h, ny)


@dataclass
class Column:
    """Hits per query: heights[start[i]:start[i + 1]] ascending, with their normal y."""
    start: np.ndarray
    heights: np.ndarray
    ny: np.ndarray

    def below(self, y: np.ndarray, reach: float = LAND_RAY_M) -> tuple[np.ndarray, np.ndarray]:
        """The highest hit at or below y (and above y - reach) per query: height (nan without) and normal y."""
        out, nout = np.full(len(y), np.nan), np.full(len(y), np.nan)
        H, N, S = self.heights.tolist(), self.ny.tolist(), self.start.tolist()
        for i, top in enumerate(y.tolist()):
            for k in range(S[i + 1] - 1, S[i] - 1, -1):
                if H[k] <= top + REACH_EPS:
                    if H[k] >= top - reach:
                        out[i], nout[i] = H[k], N[k]
                    break
        return out, nout

    def above(self, y: np.ndarray, reach: float = LAYER_GAP_M) -> np.ndarray:
        """The lowest hit above y and at most reach above it, per query (nan without)."""
        out = np.full(len(y), np.nan)
        H, S = self.heights.tolist(), self.start.tolist()
        for i, bottom in enumerate(y.tolist()):
            for k in range(S[i], S[i + 1]):
                if H[k] > bottom + REACH_EPS:
                    if H[k] <= bottom + reach:
                        out[i] = H[k]
                    break
        return out

    def track(self, segments: np.ndarray, first: np.ndarray, reach: float = LAYER_GAP_M):
        """Follow the surface along each run of queries segments[s]:segments[s + 1] from the height first[s]: at each
        query the highest hit within the step band around the previous height (WayHeights::profile), else the hit
        nearest to it within reach (the higher on a tie). Height nan = no support."""
        out, nout = np.full(len(self.start) - 1, np.nan), np.full(len(self.start) - 1, np.nan)
        H, N, S = self.heights.tolist(), self.ny.tolist(), self.start.tolist()
        seg = segments.tolist()
        for s, prev in enumerate(first.tolist()):
            for i in range(seg[s], seg[s + 1]):
                best, kb = reach + 1e-9, -1
                for k in range(S[i + 1] - 1, S[i] - 1, -1):
                    if abs(H[k] - prev) < STEP_BAND_M:
                        kb = k
                        break
                else:
                    for k in range(S[i], S[i + 1]):
                        d = abs(H[k] - prev)
                        if d <= best:
                            best, kb = d, k
                if kb >= 0:
                    prev = H[kb]
                    out[i], nout[i] = H[kb], N[kb]
        return out, nout


# --- the two states

@dataclass
class State:
    mesh: census.WorldMesh
    columns: Columns
    walkable: np.ndarray      # (t,) natural, land, up-facing and not steeper than the slide angle
    natural_up: np.ndarray    # (t,) natural, land, up-facing: the slope histogram
    solid_up: np.ndarray      # (t,) collision, up-facing: the slope histogram of every surface


def collision_mask(mesh: census.WorldMesh) -> np.ndarray:
    ok = np.array([m.get('group') != 5 and not m.get('disable_collision') for m in mesh.materials] or [True])
    return ok[mesh.material] if len(mesh) else np.zeros(0, bool)


def load_state(base: Path, world: str, content: Path | None, region, sea: float | None) -> State:
    mesh = census.load_world(base, world, content, region)
    P = mesh.positions
    cls = census.material_classes(mesh.materials)[mesh.material] if len(mesh) else np.zeros(0, str)
    _, ny = census.triangle_areas(P)
    land = P.mean(1)[:, 1] >= sea if sea is not None else np.ones(len(P), bool)
    natural_up = np.isin(cls, wc.NATURAL) & land & (ny > census.UP_NORMAL_Y)
    walkable = natural_up & (ny >= math.cos(math.radians(SLIDE_ANGLE_DEG)) - 1e-9)
    solid = collision_mask(mesh)
    return State(mesh, Columns(P[solid]), walkable, natural_up, solid & (ny > census.UP_NORMAL_Y))


def ring(region: set[tuple[int, int]]) -> set[tuple[int, int]]:
    return {(x + dx, z + dz) for x, z in region for dx in (-1, 0, 1) for dz in (-1, 0, 1)}


def chunk_of(gx_m, gz_m) -> np.ndarray:
    """Chunk (x, z) of Gothic metres."""
    return np.stack([np.floor(np.asarray(gx_m) / census.CHUNK_M), np.floor(np.asarray(gz_m) / census.CHUNK_M)],
                    -1).astype(np.int64)


def chunk_name(c) -> str:
    return f'{int(c[0])}_{int(c[1])}'


def chunk_keys(chunks) -> np.ndarray:
    chunks = np.asarray(list(chunks), np.int64).reshape(-1, 2)
    return Columns.key(chunks[:, 0], chunks[:, 1])


def in_chunks(chunks: np.ndarray, which) -> np.ndarray:
    """Which rows of chunks (n, 2) are in the set which; all of them for None."""
    if which is None:
        return np.ones(len(chunks), bool)
    return np.isin(chunk_keys(chunks), chunk_keys(which))


in_region = in_chunks


def r(v, digits=3):
    return None if v is None or (isinstance(v, float) and math.isnan(v)) else round(float(v), digits)


def stats(d: np.ndarray) -> dict:
    """Signed min and max and percentiles of |d| over the finite values."""
    d = d[np.isfinite(d)]
    if not len(d):
        return {'n': 0, 'nonzero': 0, 'min': None, 'max': None, 'abs_p50': None, 'abs_p90': None, 'abs_p99': None,
                'abs_max': None}
    a = np.abs(d)
    p = np.percentile(a, (50, 90, 99))
    return {'n': len(d), 'nonzero': int((d != 0).sum()), 'min': r(d.min()), 'max': r(d.max()),
            'abs_p50': r(p[0]), 'abs_p90': r(p[1]), 'abs_p99': r(p[2]), 'abs_max': r(a.max())}


def slope_deg(ny) -> np.ndarray:
    return np.degrees(np.arccos(np.clip(ny, -1, 1)))


# --- parts of the check

def check_waypoints(base: Path, world: str, A: State, B: State, region, tol: float):
    net = json.loads((base / 'worlds' / world / 'waynet.json').read_text())['waynet']
    points = net['points']
    pos = np.array([p['position'] for p in points], np.float64).reshape(-1, 3) / 100.0
    gx, gy, gz = -pos[:, 0], pos[:, 1], pos[:, 2]
    colA, colB = A.columns.column(gx, gz), B.columns.column(gx, gz)
    ga, _ = colA.below(gy)
    gb, _ = colB.below(gy)
    upA, upB = colA.above(gy), colB.above(gy)
    chunks = chunk_of(pos[:, 0], pos[:, 2])
    inside = in_region(chunks, region)
    diff = gb - ga
    rows, over, buried, lost = [], [], [], []
    for i in np.nonzero(inside)[0]:
        rows.append(row := {'name': points[i]['name'], 'chunk': chunk_name(chunks[i]),
               'position_cm': [round(v, 1) for v in points[i]['position']], 'ground_a_m': r(ga[i]),
               'ground_b_m': r(gb[i]), 'change_m': r(diff[i])})
        if np.isfinite(diff[i]) and abs(diff[i]) > tol:
            over.append(row)
        elif np.isfinite(ga[i]) != np.isfinite(gb[i]):
            lost.append(row)
        if np.isfinite(upB[i]) and not (np.isfinite(upA[i]) and upA[i] <= upB[i]):
            buried.append({**row, 'surface_b_m': r(upB[i]), 'depth_m': r(upB[i] - gy[i])})
    over.sort(key=lambda w: -abs(w['change_m']))
    buried.sort(key=lambda w: -w['depth_m'])
    report = {'points': int(inside.sum()), 'with_ground_a': int(np.isfinite(ga[inside]).sum()),
              'with_ground_b': int(np.isfinite(gb[inside]).sum()), 'change_m': stats(diff[inside]),
              'over_tolerance': over, 'below_surface': buried, 'ground_lost_or_found': lost, 'rows': rows,
              'chunks_changed': sorted({w['chunk'] for w in over + buried + lost}
                                       | {chunk_name(chunks[i]) for i in np.nonzero(inside & (diff != 0)
                                                                                    & np.isfinite(diff))[0]})}
    return report, points, pos, (ga, gb), net['edges']


def check_edges(points, pos, grounds, edges, A: State, B: State, region, tol: float):
    ga, gb = grounds
    names = [p['name'] for p in points]
    chunks = chunk_of(pos[:, 0], pos[:, 2])
    inside = in_region(chunks, region)
    E = [(a, b) for a, b in edges if inside[a] or inside[b]]
    seg = [0]
    X, Z = [], []
    for a, b in E:
        pa, pb = pos[a], pos[b]
        length = math.hypot(*(pb[[0, 2]] - pa[[0, 2]]))
        n = max(1, math.ceil(length / PROFILE_SPACING_M - 1e-9))
        t = np.minimum(np.arange(n + 1) * PROFILE_SPACING_M, length) / length if length > 0 else np.zeros(1)
        X.append(-(pa[0] + (pb[0] - pa[0]) * t))
        Z.append(pa[2] + (pb[2] - pa[2]) * t)
        seg.append(seg[-1] + len(t))
    if not E:
        return {'edges': 0, 'samples': 0, 'height_change_m': stats(np.zeros(0)), 'changed': 0, 'failed': [],
                'rows': [], 'chunks_changed': []}
    X, Z, seg = np.concatenate(X), np.concatenate(Z), np.array(seg)
    start = np.array([a for a, _ in E])
    first_a = np.where(np.isfinite(ga[start]), ga[start], pos[start, 1])
    first_b = np.where(np.isfinite(gb[start]), gb[start], pos[start, 1])
    ha, na = A.columns.column(X, Z).track(seg, first_a)
    hb, nb = B.columns.column(X, Z).track(seg, first_b)
    sa, sb = slope_deg(na), slope_deg(nb)
    rows, worst = [], []
    for e, (a, b) in enumerate(E):
        s0, s1 = seg[e], seg[e + 1]
        d = np.abs(hb[s0:s1] - ha[s0:s1])
        d = d[np.isfinite(d)]
        dmax = float(d.max()) if len(d) else math.nan
        worst.append(dmax)

        def profile(h, s):
            step = np.abs(np.diff(h))
            step = step[np.isfinite(step)]
            return (float(np.nanmax(s)) if np.isfinite(s).any() else math.nan,
                    float(step.max()) if len(step) else 0.0, int((~np.isfinite(h)).sum()))
        slope_a, step_a, gap_a = profile(ha[s0:s1], sa[s0:s1])
        slope_b, step_b, gap_b = profile(hb[s0:s1], sb[s0:s1])
        why = []
        if dmax > tol:
            why.append(f'height {dmax:.2f} m')
        if slope_b > SLIDE_ANGLE_DEG and not slope_a > SLIDE_ANGLE_DEG:
            why.append(f'slope {slope_b:.0f}°')
        if step_b > STEP_HEIGHT_M and not step_a > STEP_HEIGHT_M:
            why.append(f'step {step_b:.2f} m')
        if gap_b > gap_a:
            why.append(f'no support at {gap_b - gap_a} samples')
        differs = bool(why) or dmax > 0 or slope_a != slope_b or step_a != step_b or gap_a != gap_b
        rows.append({'from': names[a], 'to': names[b], 'chunk': chunk_name(chunks[a]),
                     'length_m': r(math.hypot(*(pos[b][[0, 2]] - pos[a][[0, 2]])), 2),
                     'height_change_m': r(dmax), 'slope_a_deg': r(slope_a, 1), 'slope_b_deg': r(slope_b, 1),
                     'step_a_m': r(step_a), 'step_b_m': r(step_b), 'no_support_a': gap_a,
                     'no_support_b': gap_b, 'differs': differs, 'fails': why})
    failed = sorted((f for f in rows if f['fails']), key=lambda f: -(f['height_change_m'] or 0))
    changed = [f for f in rows if f['differs']]
    return {'edges': len(E), 'samples': len(X), 'height_change_m': stats(np.array(worst)),
            'changed': len(changed), 'failed': failed, 'rows': rows,
            'chunks_changed': sorted({f['chunk'] for f in changed})}


def layer_tops(L: census.Layers, gap: float = LAYER_GAP_M) -> tuple[np.ndarray, np.ndarray]:
    """Per layer of every cell: the cell (n, 2) and the layer's top height."""
    if not len(L.heights):
        return np.zeros((0, 2), np.int64), np.zeros(0)
    cell = np.repeat(np.arange(len(L.cells)), np.diff(L.start))
    first = np.zeros(len(L.heights), bool)
    first[L.start[:-1]] = True
    new = first | np.r_[False, np.diff(L.heights) > gap]
    last = np.r_[np.nonzero(new)[0][1:] - 1, len(L.heights) - 1]
    return L.cells[cell[last]], L.heights[last]


def nearest_layer(ka, ha, kb, hb) -> np.ndarray:
    """For every (key, height) of A the height of B's layer in the same cell nearest to it (the higher on a tie);
    nan where B has none in that cell."""
    out = np.full(len(ka), np.nan)
    if not len(ka) or not len(kb):
        return out
    ob = np.lexsort((hb, kb))
    kb, hb = kb[ob], hb[ob]
    lo = np.searchsorted(kb, ka, 'left')
    n = np.searchsorted(kb, ka, 'right') - lo
    ia = np.repeat(np.arange(len(ka)), n)
    ib = np.repeat(lo, n) + np.arange(n.sum()) - np.repeat(np.cumsum(n) - n, n)
    d = np.abs(hb[ib] - ha[ia])
    o = np.lexsort((-hb[ib], d, ia))
    ia, ib = ia[o], ib[o]
    keep = np.r_[True, ia[1:] != ia[:-1]]
    out[ia[keep]] = hb[ib[keep]]
    return out


def check_ground(A: State, B: State, region, tol: float, cell: float = 1.0):
    def tops(S: State):
        L = census.surface_layers(S.mesh.positions[S.walkable], cell)
        c, h = layer_tops(L)
        ch = chunk_of(-(c[:, 0] + 0.5) * cell, (c[:, 1] + 0.5) * cell) if len(c) else np.zeros((0, 2), np.int64)
        keep = in_region(ch, region)
        return Columns.key(c[keep, 0], c[keep, 1]) if len(c) else np.zeros(0, np.int64), h[keep], ch[keep]

    ka, ha, ca = tops(A)
    kb, hb, cb = tops(B)
    nb = nearest_layer(ka, ha, kb, hb)
    na = nearest_layer(kb, hb, ka, ha)
    d = nb - ha
    matched = np.abs(d) <= LAYER_GAP_M
    d = np.where(matched, d, np.nan)
    vanished = ~matched
    appeared = ~(np.abs(na - hb) <= LAYER_GAP_M)
    area = cell * cell
    rows = {}

    def row(name):
        return rows.setdefault(name, {'chunk': name, 'layers_a': 0, 'changed': 0, 'over_tolerance_m2': 0.0,
                                      'abs_max_m': 0.0, 'vanished': 0, 'appeared': 0})

    changed = (np.isfinite(d) & (d != 0)) | vanished
    keys_a, keys_b = chunk_keys(ca), chunk_keys(cb)
    for c in np.unique(ca[changed], axis=0):
        s = keys_a == chunk_keys([c])[0]
        dd = d[s]
        w = row(chunk_name(c))
        w['layers_a'] = int(s.sum())
        w['changed'] = int(changed[s].sum())
        w['over_tolerance_m2'] = float((np.abs(dd) > tol).sum() * area)
        w['abs_max_m'] = r(np.nanmax(np.abs(dd)) if np.isfinite(dd).any() else 0.0)
        w['vanished'] = int(vanished[s].sum())
    for c in np.unique(cb[appeared], axis=0):
        row(chunk_name(c))['appeared'] = int((appeared & (keys_b == chunk_keys([c])[0])).sum())
    return {'cell_m': cell, 'layers_a': len(ha), 'layers_b': len(hb), 'change_m': stats(d),
            'over_tolerance_m2': float((np.abs(d) > tol).sum() * area),
            'vanished_layers': int(vanished.sum()), 'appeared_layers': int(appeared.sum()),
            'chunks': sorted(rows.values(), key=lambda w: w['chunk'])}


def check_objects(base: Path, world: str, content: Path | None, A: State, B: State, region):
    vobs, layers = census.load_vobs(base, world, content)
    pos = np.array([v.position for v in vobs], np.float64).reshape(-1, 3) / 100.0
    chunks = chunk_of(pos[:, 0], pos[:, 2])
    inside = in_region(chunks, region)
    vobs = [v for v, k in zip(vobs, inside) if k]
    pos, chunks = pos[inside], chunks[inside]
    gx, gz = -pos[:, 0], pos[:, 2]
    colA, colB = A.columns.column(gx, gz), B.columns.column(gx, gz)
    ga, _ = colA.below(pos[:, 1] + OBJECT_REACH_M)
    gb, _ = colB.below(pos[:, 1] + OBJECT_REACH_M)
    d = gb - ga
    moved = []
    for i in np.nonzero((np.abs(d) > OBJECT_TOLERANCE_M) | (np.isfinite(ga) != np.isfinite(gb)))[0]:
        v = vobs[i]
        moved.append({'id': v.id, 'key': v.key, 'source': v.source, 'kind': v.kind, 'visual': v.visual,
                      'class': wc.object_class(v.visual) if v.visual else None, 'chunk': chunk_name(chunks[i]),
                      'position_cm': [round(float(c), 1) for c in v.position], 'ground_a_m': r(ga[i]),
                      'ground_b_m': r(gb[i]), 'change_m': r(d[i])})
    moved.sort(key=lambda o: -abs(o['change_m'] if o['change_m'] is not None else 1e9))
    return {'objects': len(vobs), 'with_ground_a': int(np.isfinite(ga).sum()), 'change_m': stats(d),
            'moved': moved, 'vob_layers': layers,
            'chunks_changed': sorted({chunk_name(c) for c in chunks[np.isfinite(d) & (d != 0)]}
                                     | {o['chunk'] for o in moved})}


def slope_histogram(A: State, B: State, changed: set[tuple[int, int]]):
    """Area (m²) by face angle in the changed chunks: walkable ground (natural) and every up-facing collision
    surface (stairs, floors, decks too)."""
    out = {'bins_deg': list(SLOPE_BINS), 'chunks': sorted(chunk_name(c) for c in changed)}
    for part in ('natural', 'all'):
        for label, S in (('a_m2', A), ('b_m2', B)):
            sel = (S.natural_up if part == 'natural' else S.solid_up) & in_chunks(S.mesh.chunk, changed)
            area, ny = census.triangle_areas(S.mesh.positions[sel])
            h, _ = np.histogram(slope_deg(ny), bins=SLOPE_BINS, weights=area)
            out.setdefault(part, {})[label] = [round(float(v), 1) for v in h]
    return out


def walk_check(base: Path, world: str, content: Path | None, region=None, path_tol: float = 0.05,
               ground_tol: float = 0.30) -> dict:
    t0, c0 = time.time(), time.process_time()
    load = ring(region) if region is not None else None
    sea = census.sea_level(base, world)
    A = load_state(base, world, None, load, sea)
    B = load_state(base, world, content, load, sea)
    changed = {f.coords for f in B.mesh.files if f.from_content and (region is None or f.coords in region)}
    R = {'world': world, 'base': str(base), 'content': str(content) if content else None,
         'region': sorted(chunk_name(c) for c in region) if region is not None else None,
         'sea_level_m': sea, 'content_chunks': sorted(chunk_name(c) for c in changed),
         'limits': {'path_tolerance_m': path_tol, 'ground_tolerance_m': ground_tol,
                    'object_tolerance_m': OBJECT_TOLERANCE_M, 'step_height_m': STEP_HEIGHT_M,
                    'slide_angle_deg': SLIDE_ANGLE_DEG, 'profile_spacing_m': PROFILE_SPACING_M,
                    'layer_gap_m': LAYER_GAP_M}}
    R['waypoints'], points, pos, grounds, edges = check_waypoints(base, world, A, B, region, path_tol)
    R['edges'] = check_edges(points, pos, grounds, edges, A, B, region, path_tol)
    R['ground'] = check_ground(A, B, region, ground_tol)
    R['objects'] = check_objects(base, world, content, A, B, region)
    R['slope_histogram'] = slope_histogram(A, B, changed)
    w, e, g = R['waypoints'], R['edges'], R['ground']
    R['failures'] = {'waypoints_over_tolerance': len(w['over_tolerance']),
                     'waypoints_below_surface': len(w['below_surface']),
                     'waypoints_ground_lost': len(w['ground_lost_or_found']),
                     'edges': len(e['failed']), 'ground_over_tolerance_m2': g['over_tolerance_m2'],
                     'ground_vanished_layers': g['vanished_layers']}
    R['passed'] = not any(R['failures'].values())
    R['chunks_with_differences'] = sorted(set(w['chunks_changed']) | set(e['chunks_changed'])
                                          | {c['chunk'] for c in g['chunks']} | set(R['objects']['chunks_changed']))
    R['seconds'] = round(time.time() - t0, 1)
    R['cpu_seconds'] = round(time.process_time() - c0, 1)
    return R


# --- report

table, num = census.table, census.num


def cut(rows: list, n: int = LIST_ROWS) -> tuple[list, str]:
    return rows[:n], (f'{len(rows) - n} more in the JSON.\n' if len(rows) > n else '')


def stat_row(label, s) -> list:
    return [label, s['n'], s['nonzero'], s['min'], s['max'], s['abs_p50'], s['abs_p90'], s['abs_p99'], s['abs_max']]


STAT_HEAD = ['quantity', 'n', 'non-zero', 'min', 'max', '|p50|', '|p90|', '|p99|', '|max|']


def markdown(R: dict) -> str:
    L = R['limits']
    out = [f'# Walkability check of {R["world"]}: base (A) against base + content (B)\n']
    region = 'whole world' if R['region'] is None else f'{len(R["region"])} chunks: {", ".join(R["region"])}'
    out.append(f'Base `{R["base"]}`; content `{R["content"]}`; region {region}. Content chunks: '
               f'{", ".join(R["content_chunks"]) or "none"}.')
    verdict = 'PASSED' if R['passed'] else 'FAILED: ' + ', '.join(f'{k.replace("_", " ")} {v:g}'
                                                                  for k, v in R['failures'].items() if v)
    out.append(f'**{verdict}.** Chunks with any difference: {", ".join(R["chunks_with_differences"]) or "none"}.\n')
    out.append(f'Limits: path tolerance {L["path_tolerance_m"]} m (waypoints, edges); walkable ground tolerance '
               f'{L["ground_tolerance_m"]} m; objects listed over {L["object_tolerance_m"]} m. Engine limits of '
               f'`OpenGothic/common/game/movealgo.cpp`: `MoveAlgo::stepHeight()` = guild `step_height`, '
               f'{L["step_height_m"]} m for humans; `MoveAlgo::slideAngle()` = guild `slide_angle`, '
               f'{L["slide_angle_deg"]:g}° for humans. Edge profiles every {L["profile_spacing_m"]} m '
               f'(`WayHeights::Profile`, `wayheights.h`). Heights in metres, Gothic axes.\n')

    w = R['waypoints']
    out.append('## Waypoints\n')
    out.append(f'{w["points"]:,} waypoints; ground found by a ray straight down (`WayHeights::adjust` → `landRay`) '
               f'in A {w["with_ground_a"]:,}, in B {w["with_ground_b"]:,}. Ground A, ground B and the change of every '
               f'waypoint: `waypoints.rows` of the JSON.\n')
    out.append(table(STAT_HEAD, [stat_row('ground B − A', w['change_m'])]))
    for title, rows, extra in (('Over the path tolerance', w['over_tolerance'], []),
                               ('Stored position below the surface of B (the ray starts inside the ground)',
                                w['below_surface'], ['surface_b_m', 'depth_m']),
                               ('Ground found in one state only', w['ground_lost_or_found'], [])):
        if rows:
            shown, more = cut(rows)
            out.append(f'{title}: {len(rows)}.\n')
            out.append(table(['waypoint', 'chunk', 'position cm', 'ground A', 'ground B', 'change'] + extra,
                             [[x['name'], x['chunk'], x['position_cm'], x['ground_a_m'], x['ground_b_m'],
                               x['change_m']] + [x[k] for k in extra] for x in shown]) + more)

    e = R['edges']
    out.append('## Waynet edges\n')
    out.append(f'{e["edges"]:,} edges, {e["samples"]:,} profile samples; {e["changed"]} edges differ, '
               f'{len(e["failed"])} fail. An edge fails on a height change over the path tolerance, on a face '
               f'steeper than the slide angle or a step over the step height in B where A had none, or on more '
               f'samples without support. Height change, slope, step and gaps of every edge in A and B: `edges.rows` of '
               f'the JSON; below, the failing ones.\n')
    out.append(table(STAT_HEAD, [stat_row('largest |B − A| per edge', e['height_change_m'])]))
    if e['failed']:
        shown, more = cut(e['failed'])
        out.append(table(['from', 'to', 'chunk', 'length', 'height change', 'slope A°', 'slope B°', 'step A',
                          'step B', 'no support A/B', 'fails on'],
                         [[f['from'], f['to'], f['chunk'], f['length_m'], f['height_change_m'], f['slope_a_deg'],
                           f['slope_b_deg'], f['step_a_m'], f['step_b_m'], f'{f["no_support_a"]}/{f["no_support_b"]}',
                           '; '.join(f['fails'])] for f in shown]) + more)

    g = R['ground']
    out.append('## Walkable ground\n')
    out.append(f'Ground and rock, above sea, up-facing and not steeper than {L["slide_angle_deg"]:g}°; layers on a '
               f'{g["cell_m"]} m grid (A {g["layers_a"]:,}, B {g["layers_b"]:,}). Each layer top of A matched to '
               f'the nearest of B within {L["layer_gap_m"]} m. Over the ground tolerance: '
               f'{num(g["over_tolerance_m2"])} m²; vanished layers {g["vanished_layers"]:,}, appeared '
               f'{g["appeared_layers"]:,}.\n')
    out.append(table(STAT_HEAD, [stat_row('layer top B − A', g['change_m'])]))
    if g['chunks']:
        out.append(table(['chunk', 'layers A', 'changed', 'over tolerance m²', '|max| m', 'vanished', 'appeared'],
                         [[c['chunk'], c['layers_a'], c['changed'], num(c['over_tolerance_m2']), c['abs_max_m'],
                           c['vanished'], c['appeared']] for c in g['chunks']]))

    o = R['objects']
    out.append('## Objects\n')
    out.append(f'{o["objects"]:,} vobs of B with children; ground under them found in A for {o["with_ground_a"]:,}. '
               f'{len(o["moved"])} have ground moved by more than {L["object_tolerance_m"]} m (or found in one state '
               f'only): the input of the re-seat tool (`{R["world"]}_walk_objects.json`).\n')
    out.append(table(STAT_HEAD, [stat_row('ground B − A', o['change_m'])]))
    if o['moved']:
        shown, more = cut(o['moved'])
        out.append(table(['id / key', 'source', 'visual', 'chunk', 'position cm', 'ground A', 'ground B', 'change'],
                         [[x['id'] if x['id'] is not None else x['key'], x['source'], x['visual'], x['chunk'],
                           x['position_cm'], x['ground_a_m'], x['ground_b_m'], x['change_m']] for x in shown]) + more)

    s = R['slope_histogram']
    out.append('## Slope histogram of the changed chunks\n')
    if s['chunks']:
        out.append(f'Chunks {", ".join(s["chunks"])}: area m² of up-facing (normal y > {census.UP_NORMAL_Y}) faces '
                   f'by their angle from up — ground and rock above sea, and every collision surface. Walking stops '
                   f'above {L["slide_angle_deg"]:g}°.\n')
        bins = s['bins_deg']
        out.append(table(['surface', 'state'] + [f'{a}-{b}°' for a, b in itertools.pairwise(bins)],
                         [[label, st] + [num(v) for v in s[part][key]]
                          for part, label in (('natural', 'ground and rock'), ('all', 'every collision surface'))
                          for st, key in (('A', 'a_m2'), ('B', 'b_m2'))]))
    else:
        out.append('No content chunks in the region.\n')
    out.append(f'Check took {R["seconds"]} s ({R["cpu_seconds"]} s of CPU).\n')
    return '\n'.join(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('world')
    ap.add_argument('--base')
    ap.add_argument('--content', type=Path, required=True)
    where = ap.add_mutually_exclusive_group()
    where.add_argument('--chunks', help='chunk names, e.g. 8_-4,8_-3')
    where.add_argument('--rect', help='X0,Z0,X1,Z1 in chunk coordinates, inclusive')
    ap.add_argument('--out', type=Path, help='write <WORLD>_walk.md, _walk.json and _walk_objects.json here')
    ap.add_argument('--path-tolerance', type=float, default=0.05, help='metres, waypoints and edges (default 0.05)')
    ap.add_argument('--ground-tolerance', type=float, default=0.30, help='metres, walkable ground (default 0.30)')
    argv = list(sys.argv[1:] if argv is None else argv)
    for k, arg in enumerate(argv[:-1]):     # '--chunks -2_-3': argparse would take the value for an option
        if arg in ('--chunks', '--rect'):
            argv[k:k + 2] = [f'{arg}={argv[k + 1]}']
            break
    a = ap.parse_args(argv)
    base = census.resolve_base(a.base)
    if not (base / 'worlds' / a.world / 'world.json').is_file():
        raise SystemExit(f'no world {a.world} in {base}')
    if not a.content.is_dir():
        raise SystemExit(f'no content folder {a.content}')
    region = census.parse_chunks(a.chunks) if a.chunks else census.parse_rect(a.rect) if a.rect else None
    R = walk_check(base, a.world, a.content, region, a.path_tolerance, a.ground_tolerance)
    text = markdown(R)
    print(text)
    if a.out:
        a.out.mkdir(parents=True, exist_ok=True)
        (a.out / f'{a.world}_walk.md').write_text(text)
        (a.out / f'{a.world}_walk.json').write_text(json.dumps(R, indent=1, default=float))
        (a.out / f'{a.world}_walk_objects.json').write_text(json.dumps(
            {'world': a.world, 'base': R['base'], 'content': R['content'],
             'tolerance_m': OBJECT_TOLERANCE_M, 'objects': R['objects']['moved']}, indent=1))
    return 0 if R['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
