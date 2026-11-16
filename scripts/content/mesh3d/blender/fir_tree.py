"""The spruce's near level of detail as arrays: trunk, whorls of first-order branches, bough cards. NumPy only.

    build(config, seed=None, scale=1.0) -> Tree

The shape is the construction table of reports/content/2026-11-trees/fir/README.md; the numbers of a variant live
in its config (fir.json is spruce A, the spruce of the forest edge), the seed moves everything that is chance:
the height of each whorl, the number, turn, length and sag of its branches, the boughs along a branch.

- **Trunk**: one straight tube from 0.4 m under the ground to the tip of the leader, root flares at the foot.
- **Branch**: a tube in the vertical plane of its azimuth; the low ones sag and turn up at the tip, the top ones
  rise. A few dead stubs under the crown carry no boughs.
- **Bough**: a card of the atlas, cut to 6 or 8 vertices, listed twice (tree_glb.py). The cards of a branch lie in
  the slope of its tier, a roof that falls outward: a fan along the branch's end with its sides drooping and
  boughs to both sides. Under the branch hang its sprays, a curtain to each side and one at the tip, each turned
  its own way about the vertical: a branch seen from the side shows the curtain, one seen end-on the turned
  sprays and the roof. A card that would go under the ground lies on it.
- **Normals of the cards** are those of the crown's hull (outward and up, more up to the top) mixed with the
  card's own, so the crown is lit as a volume and still shows its boughs.
- **COLOR_0** is the layout of tree_glb.py. The openness A is the distance from the axis against the crown's own
  outline at that height.

Axes: Z up, metres, the origin is the trunk's centre on the ground.
"""
from __future__ import annotations

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

    def __init__(self, name: str, group: str, cutout: bool):
        self.name, self.group, self.cutout = name, group, cutout
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
        return dict(name=self.name, group=self.group, cutout=self.cutout,
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


def signed_volume(position: np.ndarray, index: np.ndarray) -> float:
    a, b, c = (position[index[:, k]] for k in range(3))
    return float(np.einsum('ij,ij->i', a, np.cross(b, c)).sum() / 6)


class Fir:
    def __init__(self, config: dict, seed: int | None = None):
        self.cfg = config
        self.rng = np.random.default_rng(config['seed'] if seed is None else seed)
        self.height = float(config['height'])
        names = config['materials']
        self.bark = Buffers(names['bark'], 'WOOD', False)
        self.boughs = Buffers(names['boughs'], 'UNDEF', True)
        self.branches: list[Branch] = []
        self.inside_out = 0
        self.tubes = 0
        self.cards = {}
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
               wobble: float = .1, wood: float = 1.) -> Branch:
        b = self.cfg['branch']
        segments = int(np.clip(math.ceil(length / b['segment']), 3, 6))
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
        sides = 4 if length > 2.5 else 3
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
        self.tube(rings, around, texture, colour, branch.number)
        return branch

    # --- boughs

    def card(self, kind: str, origin, spine, normal, length: float, width: float, branch: Branch | None,
             weight, droop: float = 0., fold: float = 0., ridge: bool | None = None) -> None:
        """A bough from the atlas: its stem at origin, its spine along `spine`, facing `normal`. droop bends the
        spine down from the normal over the length, fold lowers both edges. weight: R, a number or a function of
        the share of the card's length. ridge: vertices along the spine, 8 a card instead of 6; the cell's own
        choice unless given."""
        cell = self.cfg['atlas']['cells'][kind]
        size = self.cfg['atlas']['size']
        ridge = cell['spine'] if ridge is None else ridge
        d = unit(spine)
        n = unit(np.asarray(normal, np.float64) - d * np.dot(normal, d))
        w = np.cross(n, d)
        (t1, h1), (t2, h2) = cell['sections']
        rows = [(0., ()), (t1, (h1, 0., -h1) if ridge else (h1, -h1)),
                (t2, (h2, 0., -h2) if ridge else (h2, -h2)), (1., ())]
        ground = self.cfg['cards']['ground']
        if d[2] < -.05:
            length = max(.45 * length, min(length, (origin[2] - ground) / -d[2] * 1.1))
        position, uv, flutter, share = [], [], [], []
        point, last = np.asarray(origin, np.float64), 0.
        x, y, cw, ch = cell['cell']
        for t, across in rows:
            mid = (t + last) / 2
            point = point + (d * math.cos(droop * mid) - n * math.sin(droop * mid)) * (t - last) * length
            last = t
            down = n * math.cos(droop * t) + d * math.sin(droop * t)
            for h in across or (0.,):
                position.append(point + w * h * width / 2 * math.cos(fold) - down * abs(h) * width / 2 * math.sin(fold))
                uv.append(((x + t * cw) / size, (y + ch / 2 - h * ch / 2) / size))
                flutter.append(max(t, abs(h)))
                share.append(t)
        position = np.array(position)
        position[:, 2] = np.maximum(position[:, 2], ground + .04 * np.array(flutter) * self.rng.uniform(.3, 1.))
        if ridge:
            tris = [(0, 2, 1), (0, 3, 2), (1, 2, 5), (1, 5, 4), (2, 3, 6), (2, 6, 5), (4, 5, 7), (5, 6, 7)]
        else:
            tris = [(0, 2, 1), (1, 2, 4), (1, 4, 3), (3, 4, 5)]
        tris = np.array(tris)
        face = np.cross(position[tris[:, 1]] - position[tris[:, 0]], position[tris[:, 2]] - position[tris[:, 0]])
        if np.dot(face.sum(axis=0), n) < 0:
            tris = tris[:, ::-1]
        hull_share = self.cfg['cards']['hull_share']
        fallback = np.array([math.cos(branch.azimuth), math.sin(branch.azimuth), 0.]) if branch else unit(n * (1, 1, 0))
        normals = []
        for p in position:
            hull = self.hull_normal(p, fallback)
            own = n if np.dot(n, hull) >= 0 else -n
            normals.append(unit(hull_share * hull + (1 - hull_share) * own))
        colour = np.zeros((len(position), 4))
        colour[:, 0] = [weight(t) for t in share] if callable(weight) else weight
        colour[:, 1] = branch.phase if branch else 0.
        colour[:, 2] = flutter
        colour[:, 3] = -1
        first = self.boughs.add(position, normals, uv, colour, branch.number if branch else 0)
        self.boughs.triangles(np.concatenate([tris, tris[:, ::-1]]) + first)
        self.cards[kind] = self.cards.get(kind, 0) + 1

    def hull_normal(self, p, fallback) -> np.ndarray:
        r = math.hypot(p[0], p[1])
        radial = np.array([p[0] / r, p[1] / r, 0.]) if r > .25 else fallback
        return unit(radial + (.45 + .45 * float(np.clip(p[2] / self.height, 0, 1))) * UP)

    def dress(self, branch: Branch) -> None:
        """The boughs of one living branch."""
        c, rng = self.cfg['cards'], self.rng
        atlas = self.cfg['atlas']
        radial = np.array([math.cos(branch.azimuth), math.sin(branch.azimuth), 0.])
        across = np.array([-radial[1], radial[0], 0.])
        size = float(np.clip(.5 + .5 * branch.length / self.cfg['branch']['length'][0], .5, 1.))

        def metres(kind):
            return np.array(atlas['cells'][kind]['cell'][2:]) / atlas['px_per_m']

        def roof(jitter=8.):
            slope = math.radians(lerp(c['slope_deg'][0], c['slope_deg'][1], branch.height) + rng.uniform(-jitter, jitter))
            return math.cos(slope) * UP + math.sin(slope) * radial, math.cos(slope) * radial - math.sin(slope) * UP

        # fans along the branch from its end back: the bough seen from above, its sides drooping
        reach = 1.
        while True:
            long, wide = metres('fan') * size * rng.uniform(.9, 1.1) * (1. if reach == 1. else .85)
            long = min(long, branch.length * 1.2)
            wide = min(wide, long * .8)
            start = max(0., reach - .72 * long / branch.length)
            origin, tangent = branch.at(start)
            _, end = branch.at(min(1., start + .72 * long / branch.length))
            tangent = unit(tangent + end)
            above = np.cross(np.cross(tangent, UP), tangent)
            self.card('fan', origin + above * c['fan_lift'], tangent, above, long, wide, branch,
                      lambda t, start=start, long=long: min(1., start + t * long / branch.length) * branch.weight,
                      droop=math.radians(rng.uniform(4, 14)), fold=math.radians(rng.uniform(24, 38)))
            reach = start + .25 * long / branch.length
            if reach < c['fans_from'] + .72 * long / branch.length:
                break
        # boughs to both sides, in the slope of the tier
        step = c['step'] * (.7 + .3 * size) / branch.length
        s, side = c['start'] + rng.uniform(0, step), rng.choice([-1., 1.])
        while s < .95 and branch.length >= 1.:
            origin, tangent = branch.at(s)
            normal, down = roof()
            sparse = s < c['start'] + .12 and rng.uniform() < .6
            kind = 'sparse' if sparse else rng.choice(['side', 'twig_1', 'twig_2'], p=[.5, .25, .25])
            long, wide = metres(kind) * size * rng.uniform(.8, 1.15)
            sweep = math.radians(rng.uniform(15, 55))
            spine = unit(math.cos(sweep) * side * across + math.sin(sweep) * down + tangent * rng.uniform(.1, .5))
            roll = math.radians(rng.uniform(-18, 18))
            normal = unit(normal * math.cos(roll) + np.cross(spine, normal) * math.sin(roll))
            self.card(kind, origin, spine, normal, long, wide, branch, s * branch.weight,
                      droop=math.radians(rng.uniform(8, 30)), fold=math.radians(rng.uniform(-8, 12)))
            s += step * rng.uniform(.75, 1.25)
            side = -side
        # the sprays that hang from the branch: a curtain to each side, each spray turned about the vertical
        k = c['curtain']
        drop = min(metres('hang_1')[0] * size, k['drop'][0] + k['drop'][1] * branch.length)
        hang = lerp(1., k['top_hang'], smooth((branch.height - .55) / .35))   # the top's sprays stand out, not down
        s, side = k['from'] + rng.uniform(0, .08), rng.choice([-1., 1.])
        while True:
            tip = s >= 1.
            kind = rng.choice(['hang_1', 'hang_2', 'fan'] if drop > k['long'] else ['side', 'twig_1', 'twig_2'])
            cell = metres(kind)
            origin, tangent = branch.at(min(s, 1.))
            long = drop * rng.uniform(.75, 1.15) * (1. if tip else lerp(.8, 1., min(s / .6, 1.)))
            wide = cell[1] * long / cell[0] * rng.uniform(1., 1.3)
            long = min(long, max(.25, (origin[2] - c['ground']) * 1.2))      # short over the ground, not laid on it
            turn = math.radians(rng.uniform(-k['turn_deg'], k['turn_deg']))
            face = radial if tip else side * (math.cos(turn) * across + math.sin(turn) * radial)
            spine = unit(-UP * hang + face * (rng.uniform(*k['lean']) + .6 * (1 - hang))
                         + tangent * (rng.uniform(0., .3) + 1 - hang))
            self.card(kind, origin, spine, face, long, wide, branch, min(s, 1.) * branch.weight,
                      droop=math.radians(rng.uniform(-10, 8)), ridge=False)
            if tip:
                break
            s += k['overlap'] * wide / branch.length / 2 * rng.uniform(.8, 1.2)
            side = -side

    def crown(self) -> None:
        w, b, rng = self.cfg['whorl'], self.cfg['branch'], self.rng
        top = self.height - self.cfg['leader']
        z, turn, lean = w['first'], rng.uniform(0, 6.28), rng.uniform(0, 6.28)
        low, high = np.array(b['pitch_low'], float), np.array(b['pitch_high'], float)
        while z < top + 1e-6:
            t = (z - w['first']) / (top - w['first'])
            height = z / self.height
            count = int(round(lerp(w['branches'][0], w['branches'][1], t) + rng.uniform(-.6, .6)))
            reach = lerp(b['length'][1], b['length'][0], (1 - t) ** b['profile'])
            reach *= rng.uniform(.6, .75) if rng.uniform() < w['weak'] and 0.05 < t < .9 else rng.uniform(.86, 1.06)
            turn += 2.4 + rng.uniform(-.4, .4)
            for k in range(count):
                if rng.uniform() < w['missing'] and t < .92:
                    continue
                azimuth = turn + 2 * math.pi * (k + rng.uniform(-.22, .22)) / count
                length = reach * rng.uniform(.82, 1.08) * (1 + b['asymmetry'] * math.cos(azimuth - lean))
                pitches = lerp(low, high, height ** np.array(b['pitch_rise'])) + rng.uniform(-6, 6, 3)
                r = float(self.trunk_radius(np.array(z), np.array(azimuth))) * .4
                base = np.array([r * math.cos(azimuth), r * math.sin(azimuth), z + rng.uniform(-.08, .08)])
                self.dress(self.branch(base, azimuth, length, pitches, height, wood=b['wood']))
            z += rng.uniform(*w['step']) * lerp(1., w['top_step'], t)
        # the leader: three small boughs round the straight shoot
        px = self.cfg['atlas']['px_per_m']
        for k in range(3):
            a = turn + 2.1 * k
            out = np.array([math.cos(a), math.sin(a), 0.])
            long, wide = np.array(self.cfg['atlas']['cells']['twig_1']['cell'][2:]) / px * rng.uniform(.75, .95)
            self.card(rng.choice(['twig_1', 'twig_2']), np.array([0., 0., top + rng.uniform(.05, .3)]),
                      unit(UP + out * .28), out, min(long, self.cfg['leader'] * .95), wide * .8, None, 0.,
                      droop=0., fold=0.)

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
            cards_total=sum(self.cards.values()), closed_parts=self.tubes, inside_out_parts=self.inside_out,
            scale=scale)
        return Tree(parts, metrics, self.branches)


def build(config: dict, seed: int | None = None, scale: float = 1.) -> Tree:
    return Fir(config, seed).build(scale)
