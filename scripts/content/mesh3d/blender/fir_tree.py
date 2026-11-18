"""The spruce's near level of detail as arrays: trunk, whorls of first-order branches, bough cards. NumPy only.

    build(config, seed=None, scale=1.0, pairs=False) -> Tree
    thin(tree, share, seed=0) -> Tree
    design_level(tree, level, mask, atlas) -> Tree

thin() makes a lighter copy of a built tree for the cost stand (reports/engine/2026-11-16-modern-trees/tree-cost.md):
not a level of detail whose look is accepted, only the same crown with fewer and larger cards. card_planes(), clusters() and axis_cover() measure a built tree: how far apart the planes of each bough are and how
many blades it has, how many shoots a cluster has and how far apart they stand, how much of the branches' axes is
under boughs.

The shape is the construction table of reports/content/2026-11-trees/fir/README.md; the numbers of a variant live
in its config (fir.json is spruce A, the spruce of the forest edge), the seed moves everything that is chance:
the height of each whorl, the number, turn, length and sag of its branches, the boughs along a branch.

- **Trunk**: one straight tube from 0.4 m under the ground to the tip of the leader, root flares at the foot.
- **Branch**: a tube in the vertical plane of its azimuth; the low ones sag and turn up at the tip, the top ones
  rise. Whorls, closer to each other in the top, and one or two shorter branches between two whorls. The lowest
  whorls are the skirt: full, even, their ends on the ground. A few dead stubs under the crown carry no boughs.
  The normals of a living branch look down: it is bark in the crown's own shade, the sun does not light it.
- **Bough**: a picture of the atlas at about the size it was baked at, and never one plane (frond()): two wings
  lowered from the spine like a roof, the spine sagging with the end raised, a long one bent once more at its
  middle. All blades of a bough share its stem and are one piece; every triangle is listed once for a
  two-sided material, or twice with `pairs` (tree_glb.py).
- **Cluster**: several boughs, its shoots, that read as one bough with a volume; each shoot has its own stem
  on the branch's axis. A hanging bough is a cluster (cluster()): a main shoot along the bough's axis and two
  to four smaller ones round it, each leaning off the axis to its own side and turned away from it. A
  branch's end is a hand (hand()): a narrow leader over the axis with a third blade across its wings, and two
  or three shoots beside and under it.
- **A branch's boughs** (dress()): a roof of fans along its axis from the end back to the first `cover_from`
  of its length, each over the stem of the next, their spines lifted over the wood: from above the axis is
  under shoots, bare only in the depth of the crown. Round the axis, side after side, slanting boughs and
  hanging clusters, along the branch (a curtain) and across it (seen from the branch's end); in the top of
  the crown the hanging ones stand on the branch instead, one bough each. Short boughs on the trunk itself
  between the branches are a sleeve that closes it. A card that would go under the ground lies on it.
- **Normals of the cards** are those of the crown's hull (outward and up, more up to the top) mixed with the
  blade's own, so the crown is lit as a volume and a bough's wings still differ. A cluster's shoots take more
  of their own (`cards.cluster.hull_share`): they face different sides, so the light tells them apart.
- **Chance** has two streams: the tree's own draws the skeleton and every bough there was before the clusters,
  the clusters' stream draws their other shoots, so the skeleton of a seed does not depend on them.
- **COLOR_0** is the layout of tree_glb.py. The openness A is the distance from the axis against the crown's own
  outline at that height.

Axes: Z up, metres, the origin is the trunk's centre on the ground.
"""
from __future__ import annotations

import heapq
import math
from dataclasses import dataclass, field

import numpy as np

UP = np.array((0., 0., 1.))


def unit(v) -> np.ndarray:
    v = np.asarray(v, np.float64)
    return v / max(float(np.linalg.norm(v)), 1e-12)


def smooth(x):
    x = np.clip(x, 0., 1.)
    return x * x * (3 - 2 * x)


def lerp(a, b, t):
    return a + (b - a) * t


class Buffers:
    """One material's vertices and triangles."""

    def __init__(self, name: str, group: str, cutout: bool, two_sided: bool = False):
        self.name, self.group, self.cutout, self.two_sided = name, group, cutout, two_sided
        self.position, self.normal, self.uv, self.colour, self.index, self.owner = [], [], [], [], [], []
        self.count = 0

    def add(self, position, normal, uv, colour, owner: int) -> int:
        first = self.count
        position = np.asarray(position, np.float64)
        self.position.append(position)
        self.normal.append(np.asarray(normal, np.float64))
        self.uv.append(np.asarray(uv, np.float64))
        self.colour.append(np.asarray(colour, np.float64))
        self.owner.append(np.full(len(position), owner, np.int32))
        self.count += len(position)
        return first

    def triangles(self, index) -> None:
        self.index.append(np.asarray(index, np.int64).reshape(-1, 3))

    def arrays(self, scale: float) -> dict:
        colour = np.rint(np.clip(np.concatenate(self.colour), 0, 1) * 255).astype(np.uint8)
        return dict(name=self.name, group=self.group, cutout=self.cutout, two_sided=self.two_sided,
                    position=(np.concatenate(self.position) * scale).astype(np.float32),
                    normal=np.concatenate(self.normal).astype(np.float32),
                    uv=np.concatenate(self.uv).astype(np.float32), colour=colour,
                    index=np.concatenate(self.index), owner=np.concatenate(self.owner))


@dataclass
class Branch:
    number: int
    points: np.ndarray          # (m + 1, 3) centreline
    radius: np.ndarray          # (m + 1,)
    length: float
    azimuth: float
    height: float               # of the base, share of the tree's height
    phase: float                # G
    weight: float = 1.0         # R at the tip
    arc: np.ndarray = field(default=None)
    whorl: int = -1

    def __post_init__(self):
        steps = np.linalg.norm(np.diff(self.points, axis=0), axis=1)
        self.arc = np.concatenate([[0.], np.cumsum(steps)]) / max(float(steps.sum()), 1e-9)

    def at(self, s: float):
        """Point and unit tangent at the share s of the length."""
        s = float(np.clip(s, 0, 1))
        i = int(np.clip(np.searchsorted(self.arc, s) - 1, 0, len(self.arc) - 2))
        t = (s - self.arc[i]) / max(self.arc[i + 1] - self.arc[i], 1e-9)
        return lerp(self.points[i], self.points[i + 1], t), unit(self.points[i + 1] - self.points[i])


@dataclass
class Tree:
    parts: list[dict]
    metrics: dict
    branches: list[Branch]
    cards: list[dict] = field(default_factory=list)     # role, kind, branch, first vertex and count in the boughs
    stubs: int = 0                                      # the first branches are dead stubs


def signed_volume(position: np.ndarray, index: np.ndarray) -> float:
    a, b, c = (position[index[:, k]] for k in range(3))
    return float(np.einsum('ij,ij->i', a, np.cross(b, c)).sum() / 6)


class Fir:
    def __init__(self, config: dict, seed: int | None = None, pairs: bool = False):
        self.cfg = config
        self.pairs = pairs          # a bough's triangles twice under a one-sided material: the way before two_sided
        self.rng = np.random.default_rng(config['seed'] if seed is None else seed)
        # the chance inside a cluster has a stream of its own: the tree's shape does not depend on the clusters
        self.own = np.random.default_rng([config['seed'] if seed is None else seed, 1])
        self.clusters = 0
        self.height = float(config['height'])
        names = config['materials']
        self.bark = Buffers(names['bark'], 'WOOD', False)
        self.boughs = Buffers(names['boughs'], 'UNDEF', True, two_sided=not pairs)
        self.branches: list[Branch] = []
        self.inside_out = 0
        self.tubes = 0
        self.cards = {}
        self.records: list[dict] = []
        trunk = config['trunk']
        self.roots = [(a, s) for a, s in zip(
            np.sort(self.rng.uniform(0, 2 * math.pi / trunk['roots'], trunk['roots'])
                    + np.arange(trunk['roots']) * 2 * math.pi / trunk['roots']),
            self.rng.uniform(.55, 1., trunk['roots']))]
        self.phases = list(self.rng.permutation(np.arange(1, 256)))

    # --- bark

    def bark_v(self, along, offset: float = 0.):
        """Metres along a branch to the picture's V (0 at its bottom): the picture's upper share, the bark without
        the foot, there and back, so a branch needs no seam."""
        uv = self.cfg['bark_uv']
        root = uv['root_share'] * uv['height_m']
        period = uv['height_m'] - root
        fold = np.mod((np.asarray(along, np.float64) + offset) / period, 2.)
        fold = np.where(fold > 1, 2 - fold, fold)
        return (root + fold * period) / uv['height_m']

    def trunk_radius(self, z, theta):
        t = self.cfg['trunk']
        r = t['radius'] * (1 - np.clip(z / self.height, 0, 1)) ** t['taper'] + t['top_radius']
        lobe = np.zeros_like(theta)
        for angle, strength in self.roots:
            lobe = np.maximum(lobe, strength * np.maximum(np.cos(theta - angle), 0) ** 6)
        lobe = .12 + .88 * lobe
        flare = (t['flare_radius'] - t['radius']) * np.exp(-np.maximum(z, 0) / t['flare_height'])
        return r + flare * lobe * (1 + .6 * np.clip(-z, 0, 1))

    def tube(self, rings: np.ndarray, normals: np.ndarray, uv: np.ndarray, colour: np.ndarray, owner: int,
             breaks=()) -> None:
        """rings (m, n + 1, 3): closed rings with the seam's column doubled; fans close both ends. A ring in
        `breaks` and the next one lie in one place with different V, no faces between them."""
        m, columns = rings.shape[:2]
        first = self.bark.add(rings.reshape(-1, 3), normals.reshape(-1, 3), uv.reshape(-1, 2),
                              colour.reshape(-1, 4), owner)
        tris = []
        for i in range(m - 1):
            if i in breaks:
                continue
            for k in range(columns - 1):
                a, b = i * columns + k, i * columns + k + 1
                c, d = a + columns, b + columns
                tris += [(a, b, d), (a, d, c)]
        for k in range(1, columns - 2):
            tris.append((0, k + 1, k))
            top = (m - 1) * columns
            tris.append((top, top + k, top + k + 1))
        tris = np.array(tris, np.int64)
        welded = rings.copy()
        welded[:, -1] = welded[:, 0]
        if signed_volume(welded.reshape(-1, 3), tris) < 0:
            tris = tris[:, ::-1]
        if signed_volume(welded.reshape(-1, 3), tris) <= 0:
            self.inside_out += 1
        self.tubes += 1
        self.bark.triangles(tris + first)

    def trunk(self) -> None:
        t = self.cfg['trunk']
        uv = self.cfg['bark_uv']
        root = uv['root_share'] * uv['height_m']
        period = uv['height_m'] - root
        top = self.height - self.cfg['leader']
        low = [-t['sunk'], -.15, 0., .12, .28, .5, .8]
        # the picture's lower share is the foot, once; its upper share repeats above, a doubled ring at each turn
        turns = np.round(np.arange(root + period, self.height + t['sunk'] - .3, period) - t['sunk'], 3)
        regular = np.arange(root - t['sunk'], top, t['ring_step'])
        zs = np.unique(np.round(np.concatenate([low, regular, [top, self.height]]), 3))
        zs = [z for z in zs if np.abs(turns - z).min(initial=9.) > .25]
        zs = np.sort(np.concatenate([zs, turns, turns]))
        breaks = [i for i in range(len(zs) - 1) if zs[i] == zs[i + 1]]
        theta = np.linspace(0, 2 * math.pi, t['sides'] + 1)
        zz, tt = np.meshgrid(zs, theta, indexing='ij')

        def surface(z, a):
            r = self.trunk_radius(z, a)
            return np.stack([r * np.cos(a), r * np.sin(a), z], axis=-1)

        rings = surface(zz, tt)
        eps = 1e-3
        normal = np.cross(surface(zz, tt + eps) - surface(zz, tt - eps), surface(zz + eps, tt) - surface(zz - eps, tt))
        normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
        along = zs + t['sunk']
        v = np.where(along <= root, along, root + np.mod(along - root, period)) / uv['height_m']
        v[[i for i in breaks]] = 1.           # the ring that ends a turn; its twin starts the next at the root's share
        texture = np.stack(np.broadcast_arrays(tt / (2 * math.pi), 1 - v[:, None]), axis=-1)
        colour = np.zeros((*zz.shape, 4))
        colour[..., 3] = 1
        self.tube(rings, normal, texture, colour, 0, breaks)

    def branch(self, base: np.ndarray, azimuth: float, length: float, pitches, height: float, weight: float = 1.,
               wobble: float = .1, wood: float = 1., shade: bool = False) -> Branch:
        b = self.cfg['branch']
        segments = int(np.clip(math.ceil(length / b['segment']), 2, b['segments_max']))
        s = np.linspace(0, 1, segments + 1)
        p0, pm, p1 = (math.radians(p) for p in pitches)
        pitch = p0 * (1 - s) * (1 - 2 * s) + pm * 4 * s * (1 - s) + p1 * s * (2 * s - 1)
        turn = azimuth + wobble * np.sin(s * self.rng.uniform(2, 5) + self.rng.uniform(0, 6.28)) * s
        points = [np.asarray(base, np.float64)]
        for i in range(segments):
            pi, ai = (pitch[i] + pitch[i + 1]) / 2, (turn[i] + turn[i + 1]) / 2
            step = np.array([math.cos(pi) * math.cos(ai), math.cos(pi) * math.sin(ai), math.sin(pi)]) * length / segments
            nxt = points[-1] + step
            nxt[2] = max(nxt[2], b['ground'])
            points.append(nxt)
        points = np.array(points)
        radius = (b['radius'][0] + b['radius'][1] * length) * (1 - s) ** .9 + b['tip_radius']
        branch = Branch(len(self.branches) + 1, points, radius, length, azimuth, height,
                        float(self.phases[len(self.branches) % 255]) / 255, weight)
        self.branches.append(branch)

        # the tube ends at `wood` of the length: the thin end is the stem painted on the fan that lies on it
        wood = np.linspace(0, wood, segments + 1)
        points = np.stack([np.interp(wood, branch.arc, points[:, k]) for k in range(3)], axis=1)
        radius = np.interp(wood, branch.arc, radius)
        sides = 4 if length > b['round'] else 3
        tangent = np.gradient(points, axis=0)
        tangent /= np.linalg.norm(tangent, axis=1, keepdims=True)
        side = np.cross(tangent, UP)
        side /= np.linalg.norm(side, axis=1, keepdims=True)
        top = np.cross(side, tangent)
        angle = np.linspace(0, 2 * math.pi, sides + 1)
        around = np.cos(angle)[None, :, None] * top[:, None] + np.sin(angle)[None, :, None] * side[:, None]
        rings = points[:, None] + radius[:, None, None] * around
        span = self.cfg['bark_uv']['branch_span']
        u = self.rng.uniform(0, 1) + angle / (2 * math.pi) * span
        along = self.bark_v(wood * length, self.rng.uniform(0, 4))
        texture = np.stack(np.broadcast_arrays(u[None], 1 - along[:, None]), axis=-1)
        colour = np.zeros((segments + 1, sides + 1, 4))
        colour[..., 0] = (wood * weight)[:, None]
        colour[..., 1] = branch.phase
        colour[..., 3] = -1          # by place, filled in openness()
        # a living branch is in the shade of its own boughs: its normals look down and a little aside
        self.tube(rings, around * b['shade_spread'] - UP if shade else around, texture, colour, branch.number)
        return branch

    # --- boughs

    def frond(self, kind: str, origin, spine, normal, length: float, width: float, branch: Branch | None,
              weight, role: str, vault: float = 0., bow: float = 0., rise: float = 0., twist: float = 0.,
              bend: bool = False, keel: int = 0, cluster: int = 0, rng=None) -> None:
        """A bough from the atlas: its stem at origin, its spine along `spine`, its upper side to `normal`.

        vault lowers both wings from the spine, bow is the share of the length the spine's middle sags from
        the line stem to end (below zero it arches), rise lifts the spine over the stem within the first third,
        twist turns the bough about its spine over the length, bend puts a vertex on the spine's middle, so the
        spine itself follows the bow. keel adds a third blade from the spine: 1 to the normal's side, -1 away
        from it; it shows one half of the picture. weight: R, a number or a function of the share of the
        length. role names the bough's place for the checks, cluster is the number of the cluster the bough is
        a shoot of. A blade is wound to the side its authored normals look to: the side a `flip` material
        lights by them. rng: the stream the bough's own chance is taken from."""
        rng = rng or self.rng
        cell = self.cfg['atlas']['cells'][kind]
        size = self.cfg['atlas']['size']
        d = unit(spine)
        n = unit(np.asarray(normal, np.float64) - d * np.dot(normal, d))
        w = np.cross(n, d)
        (t1, h1), (t2, h2) = cell['sections']
        ground = self.cfg['cards']['ground']
        if d[2] < -.05:
            length = max(.45 * length, min(length, (origin[2] - ground) / -d[2] * 1.1))
        half = float(rng.choice([-1., 1.]))
        rows = [(0., 0., 0), (t1, h1, 0), (t2, h2, 0), (1., 0., 0), (t2, -h2, 0), (t1, -h1, 0)]
        if bend:
            rows.append(((t1 + t2) / 2, 0., 0))
        if keel:
            rows += [(t1, half * h1, 1), (t2, half * h2, 1)]
        x, y, cw, ch = cell['cell']
        origin = np.asarray(origin, np.float64)
        position, uv, flutter, share = [], [], [], []
        for t, h, blade in rows:
            point = origin + d * (t * length) - n * (length * bow * 4 * t * (1 - t)) + n * (rise * float(smooth(t / .35)))
            turned = n * math.cos(twist * t) - w * math.sin(twist * t)
            wide = w * math.cos(twist * t) + n * math.sin(twist * t)
            reach = abs(h) * width / 2
            if blade:
                point = point + turned * (keel * reach)
            else:
                point = point + wide * (math.copysign(reach, h) * math.cos(vault)) - turned * (reach * math.sin(vault))
            position.append(point)
            uv.append(((x + cw / 2 + h * cw / 2) / size, (y + t * ch) / size) if cell.get('turn')
                      else ((x + t * cw) / size, (y + ch / 2 - h * ch / 2) / size))
            flutter.append(max(t, abs(h)))
            share.append(t)
        position = np.array(position)
        position[:, 2] = np.maximum(position[:, 2], ground + .04 * np.array(flutter) * rng.uniform(.3, 1.))
        S, A1, A2, T, B2, B1 = range(6)
        M = 6 if bend else None
        K1 = len(rows) - 2
        if bend:
            blades = [[(S, A1, M), (A1, A2, M), (A2, T, M)], [(S, M, B1), (B1, M, B2), (B2, M, T)]]
            blades += [[(S, K1, M), (K1, K1 + 1, M), (K1 + 1, T, M)]] if keel else []
        else:
            blades = [[(S, A1, A2), (S, A2, T)], [(S, T, B2), (S, B2, B1)]]
            blades += [[(S, K1, K1 + 1), (S, K1 + 1, T)]] if keel else []
        own = np.tile(n, (len(position), 1))
        faces = []
        for number, blade in enumerate(blades):
            blade = np.array(blade)
            face = np.cross(position[blade[:, 1]] - position[blade[:, 0]],
                            position[blade[:, 2]] - position[blade[:, 0]]).sum(axis=0)
            if number < 2 and np.dot(face, n) < 0:
                blade, face = blade[:, ::-1], -face
            blades[number] = blade
            faces.append(face)
            edge = (A1, A2) if number == 0 else (B2, B1) if number == 1 else (K1, K1 + 1)
            own[list(edge)] = unit(face)
        hull_share = self.cfg['cards']['cluster']['hull_share'] if cluster else self.cfg['cards']['hull_share']
        fallback = np.array([math.cos(branch.azimuth), math.sin(branch.azimuth), 0.]) if branch else unit(n * (1, 1, 0))
        normals = []
        for p, mine in zip(position, own):
            hull = self.hull_normal(p, fallback)
            normals.append(unit(hull_share * hull + (1 - hull_share) * (mine if np.dot(mine, hull) >= 0 else -mine)))
        normals = np.array(normals)
        tris = np.concatenate([blade if np.dot(face, normals[np.unique(blade)].sum(axis=0)) >= 0 else blade[:, ::-1]
                               for blade, face in zip(blades, faces)])
        colour = np.zeros((len(position), 4))
        colour[:, 0] = [weight(t) for t in share] if callable(weight) else weight
        colour[:, 1] = branch.phase if branch else 0.
        colour[:, 2] = flutter
        colour[:, 3] = -1
        first = self.boughs.add(position, normals, uv, colour, branch.number if branch else 0)
        self.boughs.triangles((np.concatenate([tris, tris[:, ::-1]]) if self.pairs else tris) + first)
        self.cards[kind] = self.cards.get(kind, 0) + 1
        self.records.append(dict(role=role, kind=kind, branch=branch.number if branch else 0, first=first,
                                 count=len(position), cluster=cluster))

    def hull_normal(self, p, fallback) -> np.ndarray:
        r = math.hypot(p[0], p[1])
        radial = np.array([p[0] / r, p[1] / r, 0.]) if r > .25 else fallback
        return unit(radial + (.45 + .45 * float(np.clip(p[2] / self.height, 0, 1))) * UP)

    def metres(self, kind: str) -> np.ndarray:
        """Length and width of an atlas cell as it was baked."""
        cell = self.cfg['atlas']['cells'][kind]
        return np.array(cell['cell'][:1:-1] if cell.get('turn') else cell['cell'][2:]) / self.cfg['atlas']['px_per_m']

    def fit(self, long: float, *families: str, rng=None) -> tuple[str, float, float]:
        """The picture whose baked length is nearest to the length wanted: its cell, the length within the
        scale a picture may be shown at, and the width that goes with it."""
        rng = rng or self.rng
        cells = self.cfg['atlas']['cells']
        family = min(families, key=lambda f: abs(math.log(long / self.metres(next(k for k in cells if k.startswith(f)))[0])))
        kind = str(rng.choice([k for k in cells if k.startswith(family)]))
        shape = self.metres(kind)
        low, high = self.cfg['cards']['scale']
        long = float(np.clip(long, low * shape[0], high * shape[0]))
        return kind, long, long * shape[1] / shape[0] * rng.uniform(1., 1.15)

    def cluster(self, branch: Branch, s: float, axis, normal, kind: str, long: float, wide: float, vault: float,
                twist: float) -> None:
        """A hanging bough at the share s of a branch: a cluster of shoots, each a bough of its own with its
        stem on the branch's axis.

        The main shoot hangs along `axis` with its upper side to `normal`. The others stand round it, each its
        own length: a shoot leans off the axis to its own side, its upper side looks away from the axis, turned
        about its spine, its wings fold back to the axis and its end hangs. In the depth of the crown, before
        `deep` of the branch's length, a cluster has the least number of shoots and they are not bent. The
        main shoot takes its chance from the tree's stream as the one bough it replaces did, the others from
        the clusters' own."""
        g, rng = self.cfg['cards']['cluster'], self.own
        self.clusters += 1
        deep = s < g['deep']
        d = unit(axis)
        n = unit(np.asarray(normal, np.float64) - d * np.dot(normal, d))
        w = np.cross(n, d)
        self.frond(kind, branch.at(s)[0], d, n, long, wide, branch, s * branch.weight, 'hang', vault=vault,
                   twist=twist, cluster=self.clusters)
        count = g['shoots'][0] if deep else int(rng.integers(g['shoots'][0], g['shoots'][1] + 1))
        start = rng.uniform(0, 2 * math.pi)
        for k in range(count - 1):
            around = start + 2 * math.pi * (k + rng.uniform(-g['around'], g['around'])) / (count - 1)
            aside = n * math.cos(around) + w * math.sin(around)
            tilt = math.radians(rng.uniform(*g['tilt_deg']))
            spine = d * math.cos(tilt) + aside * math.sin(tilt)
            face = aside * math.cos(tilt) - d * math.sin(tilt)
            roll = math.radians(rng.uniform(-g['roll_deg'], g['roll_deg']))
            face = face * math.cos(roll) + np.cross(spine, face) * math.sin(roll)
            kind, own, wide = self.fit(long * rng.uniform(*g['length']), *g['kinds'], rng=rng)
            at = float(np.clip(s + rng.uniform(-g['along_m'], g['along_m']) / branch.length, .02, .99))
            self.frond(kind, branch.at(at)[0], spine, face, own, wide, branch, at * branch.weight, 'hang',
                       vault=math.radians(rng.uniform(*g['vault_deg'])), bow=0. if deep else -rng.uniform(*g['arch']),
                       twist=math.radians(rng.uniform(-25, 25)), bend=not deep, cluster=self.clusters, rng=rng)

    def hand(self, branch: Branch, start: float, long: float) -> None:
        """The shoots beside the leader of a branch's end, of the cluster counted last: their stems on the axis
        within the leader's length, one to each side of it and a third under it. A shoot goes out and forward,
        lower than the leader, its upper side leans away from it and its end hangs. `long` is the leader's
        length, `start` the share of the branch it starts at."""
        g, rng = self.cfg['cards']['hand'], self.own
        side = float(rng.choice([-1., 1.]))
        for k in range(int(rng.integers(g['shoots'][0], g['shoots'][1] + 1))):
            at = float(np.clip(start + rng.uniform(*g['at']) * long / branch.length, .02, .99))
            origin, tangent = branch.at(at)
            across = unit(np.cross(tangent, UP))
            above = np.cross(across, tangent)
            under = k == 2
            splay = math.radians(rng.uniform(-12, 12) if under else rng.uniform(*g['splay_deg']))
            droop = math.radians(rng.uniform(*g['under_deg' if under else 'droop_deg']))
            out = tangent * math.cos(splay) + across * (side * math.sin(splay))
            spine = out * math.cos(droop) - above * math.sin(droop)
            face = above * math.cos(droop) + out * math.sin(droop)
            roll = math.radians(rng.uniform(-30, 30) if under else side * rng.uniform(*g['roll_deg']))
            face = face * math.cos(roll) + np.cross(spine, face) * math.sin(roll)
            kind, own, wide = self.fit(long * rng.uniform(*g['length']), *g['side'], rng=rng)
            self.frond(kind, origin, spine, face, own, wide, branch, at * branch.weight, 'tip',
                       vault=math.radians(rng.uniform(*g['vault_deg'])), bow=-rng.uniform(*g['arch']),
                       twist=math.radians(rng.uniform(-25, 25)), bend=True, cluster=self.clusters, rng=rng)
            side = -side

    def dress(self, branch: Branch) -> None:
        """The boughs of one living branch: a roof of fans over its axis and second-order boughs round it."""
        c, rng = self.cfg['cards'], self.rng
        length = branch.length
        top = float(smooth((branch.height - c['top'][0]) / (c['top'][1] - c['top'][0])))

        def frame(s):
            origin, tangent = branch.at(s)
            across = unit(np.cross(tangent, UP))
            return origin, tangent, across, np.cross(across, tangent)

        def axis(s):
            """The axis and, past its end, its last direction turning up."""
            point, tangent = branch.at(min(s, 1.))
            return point + unit(tangent + .25 * UP) * max(0., s - 1.) * length

        # the roof: fans along the axis from the end back to the depth of the crown, laid like shingles: each
        # starts on the wood, rises over it and ends over the middle of the next one out. The spine stays over
        # the axis; the fans differ in length, in how much they rise and in their turn about the spine
        f = c['roof']
        wanted = float(np.clip(f['length'][0] + f['length'][1] * length, *f['limits']))
        inner = max(.05, c['cover_from'] - f['before'] / length)
        reach, on, role = 1., f['on'], 'end'
        while True:
            kind, long, wide = self.fit(wanted * rng.uniform(.8, 1.1), 'fan', 'top', 'twig')
            start = max(inner, reach - on * long / length)
            if role != 'end' and start + long / length > 1.02:
                long = (1.02 - start) * length
            origin, tangent, across, above = frame(start)
            stem = origin + above * min(c['lift'], float(np.interp(start, branch.arc, branch.radius)))
            middle = axis(start + .5 * long / length)
            end = axis(start + long / length) + above * (long * (0. if role == 'end' else rng.uniform(*f['tilt'])))
            spine = unit(end - stem)
            above = unit(np.cross(np.cross(spine, UP), spine))
            thick = float(np.interp(min(1., start + .5 * long / length), branch.arc, branch.radius))
            sag = -float(np.dot(middle - (stem + end) / 2, above)) / long
            last = start <= inner + 1e-6
            lean = math.radians(rng.uniform(-f['lean_deg'], f['lean_deg']))
            if role == 'end':
                # the branch's end is a hand: a narrow leader over the axis, shoots to both sides of it
                self.clusters += 1
                kind, _, wide = self.fit(long, *c['hand']['kinds'], rng=self.own)
                self.hand(branch, start, long)
            self.frond(kind, stem, spine, above * math.cos(lean) + np.cross(spine, above) * math.sin(lean), long, wide,
                       branch, lambda t, start=start, long=long: min(1., start + t * long / length) * branch.weight,
                       role, vault=math.radians(rng.uniform(*f['vault_deg'])), bow=min(sag, 0.) - rng.uniform(*f['arch']),
                       rise=thick + f['rise'], twist=math.radians(rng.uniform(-12, 12)),
                       bend=role == 'end' or long > c['bend_from'], keel=-1 if role == 'end' else 0,
                       cluster=self.clusters if role == 'end' else 0)
            if last:
                break
            reach, on, role = start + f['overlap'] * long / length, 1., 'roof'

        # second-order boughs round the branch's axis, side after side and band after band: slanting and
        # hanging ones; a hanging spray faces across the branch or out of the crown, so a branch is clothed
        # from the side and from its end. In the top of the crown the hanging ones stand on the branch
        b = c['bough']
        longest = min(b['length'][2], b['length'][0] + b['length'][1] * length)
        step = b['step'] / length
        first = min(.5, c['from_m'] / length)
        s, side, band = first + rng.uniform(0, step), rng.choice([-1., 1.]), int(rng.integers(len(b['bands'])))
        while True:
            tip = s >= .97
            s = min(s, .98)
            origin, tangent, across, above = frame(s)
            name = 'out' if tip else b['bands'][band % len(b['bands'])]
            low, high = b['roll_deg'][name]
            hanging = name in ('along', 'out')
            standing = hanging and rng.uniform() < top     # in the top the sprays stand on the branch
            if standing:
                roll, hanging = -math.radians(rng.uniform(*b['stand_deg'])), False
                sweep = math.radians(rng.uniform(25, 55))
            else:
                roll = math.radians(rng.uniform(low, high) - (0. if hanging else b['top_lift'] * top))
                sweep = math.radians(rng.uniform(4, 24) if hanging else rng.uniform(22, 55))
            out = math.cos(roll) * side * across - math.sin(roll) * above
            spine = unit(math.cos(sweep) * out + math.sin(sweep) * tangent)
            normal = math.cos(roll) * above + math.sin(roll) * side * across
            turn = math.radians(rng.uniform(-b['turn_deg'], b['turn_deg']) + (90. if name == 'out' else 0.))
            normal = normal * math.cos(turn) + np.cross(spine, normal) * math.sin(turn)
            long = longest * lerp(.7, 1., smooth(s / .35)) * rng.uniform(.82, 1.1)
            if hanging:
                long = min(long, max(.3, (origin[2] - c['ground']) * 1.15))
            elif not standing:
                long *= lerp(1., .6, smooth((s - .55) / .45))
            if s < first + .1 and rng.uniform() < .4:
                kind, long, wide = self.fit(long, 'sparse')
            elif hanging:
                kind, long, wide = self.fit(long, *c['cluster']['kinds'])
            else:
                kind, long, wide = self.fit(long, 'side', 'twig')
            vault, bow = math.radians(rng.uniform(*b['vault_deg'])), rng.uniform(*b['bow'])
            twist = math.radians(rng.uniform(-30, 30))
            if hanging:
                rng.choice([-1, 1])         # the side of the third blade a hanging bough had: the stream stays
                self.cluster(branch, s, spine, normal, kind, long, wide, vault, twist)
            else:
                self.frond(kind, origin, spine, normal, long, wide, branch, s * branch.weight, 'side', vault=vault,
                           bow=bow, twist=twist, bend=long > c['bend_from'])
            if tip:
                break
            s += step * rng.uniform(.75, 1.25)
            side, band = -side, band + 1

    def inner(self) -> None:
        """Short boughs on the trunk itself, between the branches: a sleeve that closes the trunk. They start on
        the bark and face out of the crown; low they hang, in the top every other one rises along the trunk."""
        k, rng = self.cfg['inner'], self.rng
        top = self.height - self.cfg['leader']
        z, azimuth, number = k['from'], rng.uniform(0, 6.28), 0
        while z < top:
            t = z / self.height
            azimuth += 2.399 + rng.uniform(-.3, .3)
            radial = np.array([math.cos(azimuth), math.sin(azimuth), 0.])
            rising = number % 2 == 1 and rng.uniform() < float(smooth((t - k['rise'][0]) / (k['rise'][1] - k['rise'][0])))
            pitch = math.radians(-rng.uniform(*k['rise_deg']) if rising else rng.uniform(*k['pitch_deg']))
            spine = radial * math.cos(pitch) - UP * math.sin(pitch)
            normal = radial * math.sin(pitch) + UP * math.cos(pitch)
            turn = math.radians(rng.uniform(-k['turn_deg'], k['turn_deg']))
            normal = normal * math.cos(turn) + np.cross(spine, normal) * math.sin(turn)
            kind, long, wide = self.fit(rng.uniform(*k['length']) * lerp(1., k['top'], t), 'side', 'twig', 'top')
            r = float(self.trunk_radius(np.array(z), np.array(azimuth))) * .9
            self.frond(kind, radial * r + UP * z, spine, normal, long, wide, None, 0., 'sleeve',
                       vault=math.radians(rng.uniform(*k['vault_deg'])), bow=rng.uniform(.0, .08),
                       twist=math.radians(rng.uniform(-25, 25)))
            z += rng.uniform(.7, 1.3) / k['per_m']
            number += 1

    def crown(self) -> None:
        w, b, rng = self.cfg['whorl'], self.cfg['branch'], self.rng
        top = self.height - self.cfg['leader']
        z, turn, lean, number = w['first'], rng.uniform(0, 6.28), rng.uniform(0, 6.28), 0
        low, high = np.array(b['pitch_low'], float), np.array(b['pitch_high'], float)

        def limb(z, azimuth, length):
            height = z / self.height
            pitches = lerp(low, high, height ** np.array(b['pitch_rise'])) + rng.uniform(-6, 6, 3)
            r = float(self.trunk_radius(np.array(z), np.array(azimuth))) * .4
            base = np.array([r * math.cos(azimuth), r * math.sin(azimuth), z])
            branch = self.branch(base, azimuth, length, pitches, height, wood=b['wood'], shade=True)
            branch.whorl = number
            self.dress(branch)

        while z < top + 1e-6:
            t = (z - w['first']) / (top - w['first'])
            # the lowest whorls give the crown its width: full and even; the next ones pass to the crown's own
            even = float(np.clip(1 - (number - w['skirt'] + 1) / (w['skirt'] + 1), 0, 1))
            count = int(round(lerp(w['branches'][0], w['branches'][1], t) + rng.uniform(-.6, .6)))
            reach = lerp(b['length'][1], b['length'][0], (1 - t) ** b['profile'])
            if even > 0:
                reach = lerp(reach * rng.uniform(.9, 1.03), w['skirt_reach'] * rng.uniform(.98, 1.01), even)
            else:
                reach *= rng.uniform(.6, .75) if rng.uniform() < w['weak'] and t < .9 else rng.uniform(.88, 1.05)
            turn += 2.4 + rng.uniform(-.4, .4)
            sway = b['asymmetry'] * float(smooth(t / .25))
            for k in range(count):
                if not even and rng.uniform() < w['missing'] and t < .92:
                    continue
                azimuth = turn + 2 * math.pi * (k + rng.uniform(-.22, .22)) / count
                length = reach * lerp(rng.uniform(.84, 1.08), rng.uniform(.96, 1.), even)
                limb(z + rng.uniform(-.08, .08), azimuth, length * (1 + sway * math.cos(azimuth - lean)))
            gap = rng.uniform(*w['step']) * lerp(1., w['top_step'], t)
            # the branches between the whorls: shorter, any side
            for _ in range(int(rng.integers(w['between'][0], w['between'][1] + 1)) if t < w['between'][2] else 0):
                limb(z + gap * rng.uniform(.25, .75), rng.uniform(0, 6.28), reach * rng.uniform(.45, .72))
            z += gap
            number += 1
        # the leader: small boughs round the straight shoot
        for k in range(self.cfg['leader_boughs']):
            a = turn + 2.4 * k
            out = np.array([math.cos(a), math.sin(a), 0.])
            kind, long, wide = self.fit(self.cfg['leader'] * rng.uniform(.5, .7), 'twig')
            self.frond(kind, np.array([0., 0., top + rng.uniform(-.1, .4)]), unit(UP + out * rng.uniform(.3, .6)), out,
                       long, wide, None, 0., 'leader', vault=math.radians(rng.uniform(20, 35)),
                       twist=math.radians(rng.uniform(-25, 25)))

    def stubs(self) -> None:
        s, rng = self.cfg['stubs'], self.rng
        for _ in range(s['count']):
            z, azimuth = rng.uniform(*s['height']), rng.uniform(0, 6.28)
            r = float(self.trunk_radius(np.array(z), np.array(azimuth))) * .4
            self.branch(np.array([r * math.cos(azimuth), r * math.sin(azimuth), z]), azimuth,
                        r / .4 + rng.uniform(*s['length']), rng.uniform(-12, 12) + np.array([5., 0., -6.]),
                        z / self.height, weight=s['weight'], wobble=.25)

    def openness(self) -> None:
        """A by place: the distance from the axis against the crown's outline at that height."""
        cards = np.concatenate(self.boughs.position)
        r, z = np.hypot(cards[:, 0], cards[:, 1]), cards[:, 2]
        edges = np.arange(-.5, self.height + 1.5, .75)
        outline = np.array([r[(z >= a - .4) & (z < a + 1.15)].max(initial=.3) for a in edges])
        centre = edges + .375
        for buffers in (self.bark, self.boughs):
            for position, colour in zip(buffers.position, buffers.colour):
                place = colour[:, 3] < 0
                if place.any():
                    limit = np.interp(position[:, 2], centre, outline)
                    share = np.hypot(position[:, 0], position[:, 1]) / limit
                    colour[place, 3] = smooth((share[place] - .3) / .55)

    def build(self, scale: float = 1.) -> Tree:
        self.trunk()
        self.stubs()
        stubs = len(self.branches)
        self.crown()
        self.inner()
        self.openness()
        parts = [self.bark.arrays(scale), self.boughs.arrays(scale)]
        position = np.concatenate([p['position'] for p in parts])
        triangles = {p['name']: int(len(p['index'])) for p in parts}
        metrics = dict(
            triangles=sum(triangles.values()), triangles_by_material=triangles,
            budget=self.cfg['budget_tris'], vertices={p['name']: int(len(p['position'])) for p in parts},
            bbox_min_m=[round(float(v), 3) for v in position.min(axis=0)],
            bbox_max_m=[round(float(v), 3) for v in position.max(axis=0)],
            height_m=round(float(position[:, 2].max()), 3),
            crown_width_m=[round(float(np.ptp(position[:, 0])), 3), round(float(np.ptp(position[:, 1])), 3)],
            trunk_diameter_m=[round(2 * float(self.trunk_radius(np.array(z), np.linspace(0, 6.28, 64)).mean()) * scale, 3)
                              for z in (0., 1.3)],
            root_spread_m=round(2 * float(self.trunk_radius(np.array(0.), np.linspace(0, 6.28, 256)).max()) * scale, 3),
            branches=len(self.branches) - stubs, stubs=stubs, cards=dict(sorted(self.cards.items())),
            cards_total=sum(self.cards.values()),
            cards_by_role={role: sum(r['role'] == role for r in self.records)
                           for role in sorted({r['role'] for r in self.records})},
            closed_parts=self.tubes, inside_out_parts=self.inside_out, scale=scale)
        return Tree(parts, metrics, self.branches, self.records, stubs)


def build(config: dict, seed: int | None = None, scale: float = 1., pairs: bool = False) -> Tree:
    return Fir(config, seed, pairs).build(scale)


def thin(tree: Tree, share: float, seed: int = 0) -> Tree:
    """A copy of the tree with about `share` of its triangles, for measuring what fewer triangles buy.

    Both materials target the same share. The bark keeps the trunk and the dead stubs and then the longest
    branches. If the trunk and stubs exceed that share, the cards give up the excess while keeping at least
    the first card if initially affordable. The cards are kept at random, p of them, and each is scaled about
    its centre by 1 / sqrt(p): the cards cover the area all of them covered, so the crown keeps its density
    (the background through the outline of spruce A: 10.6 % full, 11.5 / 12.4 / 14.8 % at 1/2, 1/4 and 1/8)
    and about the pixels it fills, so the triangle count changes more than its overdraw. Keeping the outline's
    cards first (by A) left holes: 23-24 % at 1/4 and 1/8. The grown cards of the outline reach further out, so
    the cards are then brought back to the crown's width about the trunk's axis. Branches, phases and wind
    weights stay as they are; a card's stem leaves its branch's axis by the growth of the card."""
    if not 0 < share <= 1:
        raise ValueError(f'share {share} is not in (0, 1]')
    if share == 1:
        return tree
    rng = np.random.default_rng(seed)
    bark, boughs = (dict(p) for p in tree.parts)

    def keep_triangles(part: dict, keep: np.ndarray) -> dict:
        index = part['index'][keep]
        used = np.unique(index)
        remap = np.full(len(part['position']), -1, np.int64)
        remap[used] = np.arange(len(used))
        out = {k: (v[used] if isinstance(v, np.ndarray) and len(v) == len(part['position']) else v)
               for k, v in part.items()}
        out['index'] = remap[index]
        return out

    # bark: the trunk (owner 0) and the stubs, then the branches from the longest
    owner = bark['owner'][bark['index'][:, 0]]
    lengths = {b.number: b.length for b in tree.branches}
    bark_budget = share * len(bark['index'])
    keep = owner <= tree.stubs
    for number in sorted(lengths, key=lambda n: -lengths[n]):
        if number <= tree.stubs:
            continue
        mine = owner == number
        if keep.sum() + mine.sum() > bark_budget:
            break
        keep |= mine
    bark = keep_triangles(bark, keep)

    # cards: at random, then scaled about their centres
    first = np.array([c['first'] for c in tree.cards])
    count = np.array([c['count'] for c in tree.cards])
    card = np.searchsorted(first, boughs['index'][:, 0], side='right') - 1
    triangles = np.bincount(card, minlength=len(first))
    order = rng.permutation(len(first))
    bough_budget = share * len(boughs['index'])
    bark_excess = max(0., len(bark['index']) - bark_budget)
    spare_bough_budget = max(0., bough_budget - triangles[order[0]])
    taken = np.cumsum(triangles[order]) <= bough_budget - min(bark_excess, spare_bough_budget)
    chosen = np.zeros(len(first), bool)
    chosen[order[taken]] = True
    p = chosen.sum() / len(first)
    grow = p ** -.5
    position = boughs['position'].astype(np.float64)
    for f, n in zip(first[chosen], count[chosen]):
        centre = position[f:f + n].mean(axis=0)
        position[f:f + n] = centre + (position[f:f + n] - centre) * grow
    boughs = keep_triangles(dict(boughs, position=position), chosen[card])
    # the grown cards of the outline reach further out: the crown is brought back to its width about the axis
    width = lambda p: float(np.ptp(p[:, 0]) + np.ptp(p[:, 1]))
    narrow = width(tree.parts[1]['position']) / width(boughs['position'])
    boughs['position'] = (boughs['position'] * (narrow, narrow, 1.)).astype(np.float32)

    parts = [bark, boughs]
    everything = np.concatenate([p['position'] for p in parts])
    metrics = dict(tree.metrics, triangles=int(sum(len(p['index']) for p in parts)),
                   triangles_by_material={p['name']: int(len(p['index'])) for p in parts},
                   vertices={p['name']: int(len(p['position'])) for p in parts},
                   bbox_min_m=[round(float(v), 3) for v in everything.min(axis=0)],
                   bbox_max_m=[round(float(v), 3) for v in everything.max(axis=0)],
                   crown_width_m=[round(float(np.ptp(everything[:, 0])), 3), round(float(np.ptp(everything[:, 1])), 3)],
                   thin=dict(share=share, seed=seed, cards_kept=int(chosen.sum()), cards_share=round(float(p), 4),
                             card_growth=round(float(grow), 3), narrowed=round(narrow, 3),
                             branches_with_wood=int(len(np.unique(bark['owner'][bark['owner'] > tree.stubs])))))
    return Tree(parts, metrics, tree.branches, [], tree.stubs)


def _level_trunk(bark: dict, sides: int) -> dict:
    trunk = np.flatnonzero(bark['owner'] == 0)
    starts = np.flatnonzero(bark['uv'][trunk, 0] == 0)
    rings = trunk.reshape(len(starts), -1)
    columns = np.rint(np.linspace(0, rings.shape[1] - 1, sides + 1)).astype(int)
    if len(np.unique(columns)) != sides + 1:
        raise ValueError('Too many sides for the level trunk')
    ring = np.arange(len(rings) * (sides + 1)).reshape(len(rings), sides + 1)
    joins = np.flatnonzero(np.diff(bark['position'][rings[:, 0], 2]) > 0)
    a, b = ring[joins, :-1].ravel(), ring[joins, 1:].ravel()
    c, d = ring[joins + 1, :-1].ravel(), ring[joins + 1, 1:].ravel()
    top = ring[-1, 0]
    k = np.arange(1, sides - 1)
    caps = np.column_stack([np.full(len(k), 0), k + 1, k])
    index = np.concatenate([np.column_stack([a, b, d]), np.column_stack([a, d, c]), caps,
                            caps[:, ::-1] + top])
    used = rings[:, columns].ravel()
    return dict(bark, index=index, **{key: bark[key][used]
                                     for key in ('position', 'normal', 'uv', 'colour', 'owner')})


def _level_groups(tree: Tree, level: dict, visible: np.ndarray, starts: np.ndarray, area: np.ndarray):
    boughs = tree.parts[1]
    openness = np.maximum.reduceat(boughs['colour'][:, 3], starts) / 255
    centres = np.add.reduceat(boughs['position'], starts) / np.diff(np.r_[starts, len(boughs['position'])])[:, None]
    branches = {branch.number: branch for branch in tree.branches}
    span = level['merge_px'] * np.ptp(tree.parts[0]['position'][:, 2]) / level['px']
    groups = {}
    for number in np.flatnonzero(visible & (openness >= level['min_open'])):
        card = tree.cards[number]
        branch = branches.get(card['branch'])
        radius = np.linalg.norm(centres[number, :2])
        segment = int(radius / span)
        if level['merge'] == 'whorl' and branch is not None:
            sector = int(branch.azimuth % math.tau * 6 / math.tau)
            key = ('whorl', branch.whorl, sector, segment)
        elif level['merge'] == 'branch' and branch is not None:
            key = ('branch', branch.number, segment)
        elif card['cluster'] and area[number] < span ** 2:
            key = ('cluster', card['cluster'])
        else:
            key = ('card', number)
        groups.setdefault(key, []).append(int(number))
    return groups.values(), centres


def _atlas_samples(luma: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
    height, width = luma.shape
    return luma[np.clip((v * height).astype(int), 0, height - 1),
                np.clip((u * width).astype(int), 0, width - 1)]


def _level_reference(parts: list[dict], starts: np.ndarray, views, mask: np.ndarray, luma: np.ndarray):
    import tree_density

    bark, boughs = parts
    owner = np.searchsorted(starts, boughs['index'][:, 0], side='right') - 1
    visible = np.zeros(len(starts), bool)
    reference = []
    tone_sum = tone_count = 0
    for _, project, shape, ppm in views:
        wood = tree_density.raster(project(bark['position']), bark['index'], shape).ravel()
        nearest = wood.copy()
        winner = np.full(len(nearest), -1, np.int32)
        tone = np.zeros(len(nearest), np.float32)
        for triangle, pixel, depth, u, v in tree_density.fragments(
                project(boughs['position']), boughs['index'], shape, boughs['uv'], mask):
            np.minimum.at(nearest, pixel, depth)
            front = (depth == nearest[pixel]) & (depth < wood[pixel])
            winner[pixel[front]] = owner[triangle[front]]
            tone[pixel[front]] = _atlas_samples(luma, u[front], v[front])
        # A later, nearer fragment can replace a winner. Pixels that show only bark have no card.
        visible[winner[winner >= 0]] = True
        reference.append((np.isfinite(nearest), shape, ppm))
        tone_sum += tone.sum(dtype=np.float64)
        tone_count += np.count_nonzero(winner >= 0)
    return visible, reference, tone_sum / tone_count


def _level_stencils(bark: dict, boughs: dict, triangle_options: np.ndarray, views, reference,
                    mask: np.ndarray, luma: np.ndarray):
    import tree_density

    pieces, targets, filled, weights = [], [], [], []
    size = int(triangle_options[-1]) + 1
    total_rays = sum(np.prod(shape) for _, _, shape, _ in views)
    tone_sum, tone_count = np.zeros(size), np.zeros(size)
    offset = 0
    for (_, project, shape, ppm), (target, _, _) in zip(views, reference):
        wood = np.isfinite(tree_density.raster(project(bark['position']), bark['index'], shape)).ravel()
        points = project(boughs['position'])
        projected = []
        for triangle, pixel, _, u, v in tree_density.fragments(points, boughs['index'], shape, boughs['uv'], mask):
            option = triangle_options[triangle]
            show = ~wood[pixel]
            projected.append(option[show] * total_rays + pixel[show] + offset)
            tone_count += np.bincount(option, minlength=size)
            tone_sum += np.bincount(option, weights=_atlas_samples(luma, u, v), minlength=size)
        pieces.append(np.unique(np.concatenate(projected)))
        targets.append(target)
        filled.append(wood)
        # Balance the views by their projected areas. Opposite views contribute equally.
        weights.append((1. / max(target.sum(), 1), np.prod(shape)))
        offset += np.prod(shape)
    option, pixel = np.divmod(np.sort(np.concatenate(pieces)), total_rays)
    cuts = np.searchsorted(option, np.arange(size + 1))
    return (np.split(pixel, cuts[1:-1]), np.concatenate(targets), np.concatenate(filled),
            np.repeat(*zip(*weights)), tone_sum / np.maximum(tone_count, 1))


def _fit_level(positive, spill: np.ndarray, initial: np.ndarray, filled: np.ndarray, weights: np.ndarray,
               group_ids: np.ndarray, costs: np.ndarray, budget: int, spill_weight: float,
               tone_weight: np.ndarray) -> list[int]:
    """Cover the full silhouette with the fewest exposed cards, charging for rays outside it."""
    filled = filled.copy()
    taken = np.zeros(group_ids[-1] + 1, bool)
    factor = tone_weight / costs
    score = (initial - spill_weight * spill) * factor
    queue = [(-value, option) for option, value in enumerate(score)]
    heapq.heapify(queue)
    selected = []
    minimum = costs.min()
    while queue and budget >= minimum:
        previous, option = heapq.heappop(queue)
        group = group_ids[option]
        if taken[group] or costs[option] > budget:
            continue
        p = positive[option]
        value = (weights[p[~filled[p]]].sum() - spill_weight * spill[option]) * factor[option]
        if value <= 0:
            continue
        if value < -previous - 1e-15:
            heapq.heappush(queue, (-value, option))
            continue
        filled[p] = True
        taken[group] = True
        budget -= costs[option]
        selected.append(option)
    return selected


def _projected_openings(coverage: np.ndarray, reference) -> np.ndarray:
    from tree_density import RAYS, level_opening

    readings = []
    offset = 0
    for target, shape, ppm in reference:
        hit = coverage[offset:offset + len(target)]
        solid = hit.reshape(shape[0] // RAYS, RAYS, shape[1] // RAYS, RAYS).sum(axis=(1, 3)) > RAYS ** 2 // 2
        readings.append(level_opening(solid, ppm))
        offset += len(target)
    return np.array(readings)


def design_level(tree: Tree, level: dict, mask: np.ndarray, atlas: np.ndarray) -> Tree:
    """Fit grouped cards to the full spruce at the threshold's projected height."""
    import tree_density

    bark, boughs = tree.parts
    starts = np.array([card['first'] for card in tree.cards])
    ends = np.r_[starts[1:], len(boughs['position'])]
    index = boughs['index']
    owners = np.searchsorted(starts, index[:, 0], side='right') - 1
    cuts = np.searchsorted(owners, np.arange(len(starts) + 1))
    points = boughs['position'][index]
    area = np.add.reduceat(np.linalg.norm(np.cross(points[:, 1] - points[:, 0], points[:, 2] - points[:, 0]),
                                         axis=1) * .5, cuts[:-1])
    views = list(tree_density.level_views(tree.parts, level['px']))
    luma = atlas[..., :3].astype(np.float32) @ np.array((.2126, .7152, .0722), np.float32)
    visible, reference, tone = _level_reference(tree.parts, starts, views, mask, luma)
    groups, centres = _level_groups(tree, level, visible, starts, area)
    trunk = _level_trunk(bark, level['trunk_sides'])
    budget = int(tree.metrics['triangles'] * level['share']) - len(trunk['index'])
    sources, members, groups_by_option, geometry, scales = [], [], [], [], []
    for group_id, group in enumerate(groups):
        source = max(group, key=area.__getitem__)
        # The largest shoot keeps its UVs, normals and wind bytes. The merged footprint sets the
        # upper size we try, with additional room to compensate for the groups omitted by the budget.
        centre = centres[source]
        points = boughs['position'][starts[source]:ends[source]] - centre
        merged = min(2., math.sqrt(area[group].sum() / area[source]))
        for grow in sorted({1., 1.18, 1.4, round(merged, 3), round(merged * 1.25, 3), round(merged * 1.6, 3)}):
            sources.append(source)
            members.append(group)
            groups_by_option.append(group_id)
            scales.append(grow)
            geometry.append((centre + points * grow).astype(np.float32))
    sources = np.array(sources)
    counts = ends[sources] - starts[sources]
    first = np.r_[0, np.cumsum(counts)]
    triangles = [index[cuts[s]:cuts[s + 1]] - starts[s] + f for s, f in zip(sources, first)]
    costs = np.diff(cuts)[sources]
    used = np.concatenate([np.arange(starts[s], ends[s]) for s in sources])
    candidates = dict(position=np.concatenate(geometry), uv=boughs['uv'][used], index=np.concatenate(triangles))
    def assemble(selected):
        positions, vertex_ids, kept_indices, cards = [], [], [], []
        first_vertex = 0
        for option in selected:
            source = sources[option]
            positions.append(geometry[option])
            vertex_ids.extend(range(starts[source], ends[source]))
            kept_indices.append(triangles[option] - first[option] + first_vertex)
            cards.append(dict(tree.cards[source], first=first_vertex, source=int(source),
                              members=members[option], growth=scales[option]))
            first_vertex += counts[option]
        part = dict(boughs, position=np.concatenate(positions), index=np.concatenate(kept_indices),
                    **{key: boughs[key][vertex_ids] for key in ('normal', 'uv', 'colour', 'owner')})
        return [trunk, part], cards

    pixels, target, filled, weights, option_tone = _level_stencils(
        trunk, candidates, np.repeat(np.arange(len(sources)), costs), views, reference, mask, luma)
    positive = [p[target[p]] for p in pixels]
    spill = np.array([weights[p[~target[p]]].sum() for p in pixels])
    initial = np.array([weights[p].sum() for p in positive])
    full = _projected_openings(target, reference)
    best = None
    group_ids = np.array(groups_by_option)
    for share in (level['target_share'], level['share']):
        limit = int(tree.metrics['triangles'] * share) - len(trunk['index'])
        fits = []
        for penalty in (.25, .65, 1.):
            for bias in (0., 12., 24.):
                tone_weight = np.exp(-bias * (option_tone / tone - 1))
                selected = _fit_level(positive, spill, initial, filled, weights, group_ids, costs,
                                      min(limit, budget), penalty, tone_weight)
                if not selected:
                    continue
                hit = filled.copy()
                for option in selected:
                    hit[pixels[option]] = True
                opening = _projected_openings(hit, reference)
                gap = np.abs(opening[:, 0] - full[:, 0]).max()
                outline = np.abs(opening[:, 1] / full[:, 1] - 1).max()
                fits.append((max(0., gap - .01), outline, gap, tuple(selected)))
        for _, outline, gap, selected in sorted(set(fits)):
            parts, cards = assemble(selected)
            rgb = np.zeros(3)
            count = 0
            for _, project, shape, _ in views:
                _, colours, rays = tree_density.level_sample(parts, project, shape, mask, atlas)
                rgb += colours
                count += rays
            delta = abs(rgb @ (.2126, .7152, .0722) / (count * tone) - 1)
            score = (max(0., gap / .01 - 1, delta / .005 - 1), outline, gap, delta)
            if best is None or score < best[0]:
                best = score, parts, cards
            if score[0] == 0:
                break
        if best is not None and best[0][0] == 0:
            break
    if best is None:
        raise ValueError(f'No crown cards fit under {level["px"]} px')
    _, parts, cards = best
    metrics = dict(tree.metrics, triangles=sum(len(part['index']) for part in parts),
                   triangles_by_material={part['name']: len(part['index']) for part in parts},
                   vertices={part['name']: len(part['position']) for part in parts},
                   designed=dict(px=level['px'], share=level['share'], cards_kept=len(cards)))
    return Tree(parts, metrics, tree.branches, cards, tree.stubs)


BLADE_DEG = 30.     # triangles whose planes are closer than this are one blade of a bough


def card_planes(tree: Tree) -> dict:
    """How far from one plane the boughs are. The spread of a bough is the largest angle between the planes of
    two of its triangles (a plane has no side: 0 to 90 degrees). Its blades are counted as the groups of its
    triangles: a triangle opens a new group when its plane is BLADE_DEG or more from the first triangle of
    every group before it. Per role: the count, the smallest spread and the least number of blades."""
    part = next(p for p in tree.parts if p['cutout'])
    position, index = part['position'].astype(np.float64), np.asarray(part['index'])
    _, first = np.unique(np.sort(index, axis=1), axis=0, return_index=True)
    index = index[np.sort(first)]
    face = np.cross(position[index[:, 1]] - position[index[:, 0]], position[index[:, 2]] - position[index[:, 0]])
    face /= np.maximum(np.linalg.norm(face, axis=1, keepdims=True), 1e-12)
    starts = np.array([card['first'] for card in tree.cards])
    owner = np.searchsorted(starts, index[:, 0], side='right') - 1
    out: dict = {}
    for number, card in enumerate(tree.cards):
        mine = face[owner == number]
        spread = math.degrees(math.acos(float(np.clip(np.abs(mine @ mine.T).min(), 0, 1))))
        blades: list = []
        for plane in mine:
            if all(abs(float(plane @ other)) <= math.cos(math.radians(BLADE_DEG)) for other in blades):
                blades.append(plane)
        role = out.setdefault(card['role'], dict(count=0, spread_min_deg=90., blades_min=len(blades)))
        role['count'] += 1
        role['spread_min_deg'] = round(min(role['spread_min_deg'], spread), 1)
        role['blades_min'] = min(role['blades_min'], len(blades))
    return out


def clusters(tree: Tree) -> dict:
    """The clusters of shoots: `hang` are the hanging boughs, `end` the hands on the branches' ends.

    A shoot's plane is the plane nearest to its vertices, its spine runs from its stem to its tip. Per cluster:
    the number of shoots, `planes` — the largest angle between the planes of two of its shoots (0 to 90
    degrees), `splay` — the largest angle between two of its spines. Of all clusters of a kind: their count,
    the least, the largest and the mean number of shoots, the shoots in all and the least of both angles."""
    position = next(p for p in tree.parts if p['cutout'])['position'].astype(np.float64)
    groups: dict = {}
    for card in tree.cards:
        if card.get('cluster'):
            groups.setdefault(card['cluster'], []).append(card)
    kinds: dict = {}
    for cards in groups.values():
        points = [position[card['first']:card['first'] + card['count']] for card in cards]
        normal = np.array([np.linalg.svd(p - p.mean(axis=0))[2][2] for p in points])
        spine = np.array([unit(p[3] - p[0]) for p in points])
        kind = 'end' if any(card['role'] == 'end' for card in cards) else 'hang'
        kinds.setdefault(kind, []).append((
            len(cards), math.degrees(math.acos(float(np.clip(np.abs(normal @ normal.T).min(), 0, 1)))),
            math.degrees(math.acos(float(np.clip((spine @ spine.T).min(), -1, 1))))))
    out = {}
    for kind, rows in sorted(kinds.items()):
        shoots, planes, splay = (np.array(column) for column in zip(*rows))
        out[kind] = dict(count=len(rows), shoots=[int(shoots.min()), int(shoots.max())],
                         shoots_mean=round(float(shoots.mean()), 2), shoots_total=int(shoots.sum()),
                         planes_min_deg=round(float(planes.min()), 1), splay_min_deg=round(float(splay.min()), 1))
    return out


def axis_cover(tree: Tree, mask: np.ndarray, start: float, step: float = .1, px: int = 100, clear: float = .01) -> dict:
    """How much of the living branches' axes is under their own boughs, seen from straight above.

    The axis of every branch is sampled each `step` metres from the share `start` of its length to its end. A
    sample is covered when a card of the same branch, there where the atlas's coverage mask is solid, lies
    `clear` metres or more over the top of the wood at that place: a card that passes through the wood, over
    the axis's centre but not over the bark, leaves it open. `covered` is the share of all samples,
    `branch_min` the share of the worst branch with five samples or more."""
    import tree_density
    part = next(p for p in tree.parts if p['cutout'])
    scale = tree.metrics['scale']
    position, uv = part['position'].astype(np.float64) / scale, part['uv']
    index = np.asarray(part['index'])
    _, first = np.unique(np.sort(index, axis=1), axis=0, return_index=True)
    index = index[np.sort(first)]
    owner = part['owner'][index[:, 0]]
    total = covered = 0
    worst = 1.
    for branch in tree.branches[tree.stubs:]:
        shares = np.arange(start * branch.length, branch.length, step) / branch.length
        if not len(shares):
            continue
        points = np.array([branch.at(s)[0] for s in shares])
        top = points[:, 2] + np.interp(shares, branch.arc, branch.radius)
        mine = index[owner == branch.number]
        low = position[np.unique(mine)][:, :2].min(axis=0) - .1
        size = np.ceil((position[np.unique(mine)][:, :2].max(axis=0) + .1 - low) * px).astype(int)
        seen = np.stack([(position[:, 0] - low[0]) * px, (position[:, 1] - low[1]) * px, -position[:, 2]], axis=1)
        height = -tree_density.raster(seen, mine, (size[1], size[0]), uv, mask)
        column = np.clip(((points[:, 0] - low[0]) * px).astype(int), 0, size[0] - 1)
        row = np.clip(((points[:, 1] - low[1]) * px).astype(int), 0, size[1] - 1)
        under = height[row, column] >= top + clear
        total += len(shares)
        covered += int(under.sum())
        if len(shares) >= 5:
            worst = min(worst, float(under.mean()))
    return dict(start=start, step_m=step, samples=total, covered=round(covered / max(total, 1), 4),
                branch_min=round(worst, 3))
