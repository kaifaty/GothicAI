"""Denser ground with relief for the landscape pilot (docs/landscape-plan.md, step В2; method note
reports/content/2026-11-landscape-pilot/b2-geometry.md).

An edit of world_edit.py. It takes the natural ground of the chunk (class `ground` of world_classes.py, without the
room copies `S:…` of caves), splits its edges until none is longer than the class's step, and moves the new surface
up and down by what lies on it:

- the folds between the base's large faces are rounded: the heights are smoothed (SMOOTH_PASSES of a Laplacian on
  the height alone, so the layout and the texture stay where they were);
- soil (meadow, forest floor): tussocks, three octaves of value noise, within about ±21 cm; a road (materials with
  PATH) takes ROAD_SHARE of it.

The first probe (the note, "Проба земли 1") had ruts of 6 cm and tussocks of a few centimetres at twice this
density: nothing of it showed in the game. Ruts and fine texture belong to the materials (step В3).

What stays where it was: every vertex on an edge the chunk shares with something else — the chunk's own border
(an open edge after welding by position), rock, man-made and vegetation faces — and those edges are not split, so
no T-junction appears. The relief fades to nothing within FADE_CM of such an edge. Near the waynet the soil's relief
is cut to WAY_SHARE, so the ground under a path stays within the walk check's 5 cm (world_walk_check.py).

The chunk's vertices are glTF vertices: one per position and feature, so a seam of the texture is two rows of
vertices at the same places. Everything here is decided per position (rounded to 0.01 cm, as the check welds):
the twins of a seam are split and moved alike.

Normals: a base vertex that did not move keeps its normal; every other corner takes the smooth normal of the new
ground around its position. New vertices get _VERTEX and _FEATURE -1 (world_bpy.bake_light gives them the baked
light of the base around them).

The numbers of the relief are estimates, not measured on a reference (the note says so); a clip corrects them.
"""
import json
import math

import bmesh
from mathutils import Vector
from mathutils.kdtree import KDTree

from world_bpy import gothic_name, to_gothic
from world_classes import MATERIAL_RULES, first

STEP_CM = 110.0          # no ground edge longer than this: 55-110 cm after halving, 3-5 triangles per m²
PASSES = 8
WELD_CM = 0.02           # twins of a seam after a split: a float step is 0.004 cm at 400 m from the origin
FADE_CM = 150.0          # the relief reaches full height this far from a fixed edge
WAY_NEAR_CM = 50.0       # soil relief is cut to WAY_SHARE closer than this to a waynet edge ...
WAY_FAR_CM = 200.0       # ... and is full beyond this
WAY_SHARE = 0.15
WAY_LIMIT_CM = 4.0       # by a waynet edge nothing moves further than this (the walk check allows 5)
# (wavelength cm, amplitude cm) of the tussocks: together they tilt the ground by no more than about 20°
SOIL = ((600.0, 14.0), (250.0, 6.0), (110.0, 1.5))
ROAD_SHARE = 0.2         # of the tussocks on a road
SMOOTH_PASSES = 12
SMOOTH_STEP = 0.5
SLOPE_FULL_DEG = 15.0    # ground steeper than this moves less ...
SLOPE_NONE_DEG = 30.0    # ... and not at all from here: the engine's slide angle is 50°, a face must not cross it
LIMIT_CM = 25.0          # nothing moves further than this (the pilot's tolerance is 30 cm)
SEED = 20261113
PARAMETERS = {'step_cm': STEP_CM, 'fade_cm': FADE_CM, 'way_near_cm': WAY_NEAR_CM, 'way_far_cm': WAY_FAR_CM,
              'way_share': WAY_SHARE, 'way_limit_cm': WAY_LIMIT_CM, 'soil': [list(o) for o in SOIL],
              'road_share': ROAD_SHARE, 'smooth_passes': SMOOTH_PASSES, 'smooth_step': SMOOTH_STEP,
              'slope_full_deg': SLOPE_FULL_DEG, 'slope_none_deg': SLOPE_NONE_DEG, 'limit_cm': LIMIT_CM, 'seed': SEED}


def key(co):
    """A place, exactly as the check welds it (chunk_glb.position_keys): two base vertices a hair apart are two
    places, and the edge between such rows counts as open and is left alone."""
    return (co.x + 0.0, co.y + 0.0, co.z + 0.0)


def smoothstep(t):
    t = min(1.0, max(0.0, t))
    return t * t * (3.0 - 2.0 * t)


def _hash(ix, iz, seed):
    n = (ix * 374761393 + iz * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 32767.5 - 1.0


def noise(x, z, seed):
    """Value noise in -1..1, the same for the same place whatever chunk it is asked from."""
    ix, iz = math.floor(x), math.floor(z)
    fx, fz = smoothstep(x - ix), smoothstep(z - iz)
    a, b = _hash(ix, iz, seed), _hash(ix + 1, iz, seed)
    c, d = _hash(ix, iz + 1, seed), _hash(ix + 1, iz + 1, seed)
    return (a + (b - a) * fx) * (1.0 - fz) + (c + (d - c) * fx) * fz


def waynet_segments(ctx, lo, hi, margin=WAY_FAR_CM + 100.0):
    """Waynet edges near the box lo..hi (Gothic x, z in cm) as ((ax, az), (bx, bz))."""
    if ctx.base_chunks is None:
        return []
    path = ctx.base_chunks.parent / 'waynet.json'
    if not path.exists():
        return []
    net = json.loads(path.read_text())['waynet']
    points = [(p['position'][0], p['position'][2]) for p in net['points']]
    edges = net['edges']
    shift = 1 if edges and max(max(e) for e in edges) >= len(points) else 0
    out = []
    for a, b in edges:
        pa, pb = points[a - shift], points[b - shift]
        if max(pa[0], pb[0]) < lo[0] - margin or min(pa[0], pb[0]) > hi[0] + margin or \
                max(pa[1], pb[1]) < lo[1] - margin or min(pa[1], pb[1]) > hi[1] + margin:
            continue
        out.append((pa, pb))
    return out


def way_distance(x, z, segments):
    """Distance in the ground plane to the nearest waynet edge, cm; inf without one."""
    best = math.inf
    for (ax, az), (bx, bz) in segments:
        dx, dz = bx - ax, bz - az
        length2 = dx * dx + dz * dz
        t = 0.0 if length2 == 0.0 else min(1.0, max(0.0, ((x - ax) * dx + (z - az) * dz) / length2))
        d = math.hypot(x - (ax + t * dx), z - (az + t * dz))
        best = min(best, d)
    return best


def flat(a, m, b):
    """Is m on the line between a and b: the triangle a, m, b has no area."""
    return (m - a).cross(b - m).length <= 1e-4 * (b - a).length_squared


def cut_polygons(bm, first_new):
    """Triangles out of what a split left with more than three corners. A triangle with two split edges stays a
    pentagon, and a general triangulation may cut the ear "end, middle, end" of a split edge: a triangle without
    area that leaves the edge whole on this side and open in the check. Here a cut never joins the two ends of a
    middle: a corner between two middles is cut off first, a quad takes its shorter sound diagonal."""
    stack = [f for f in bm.faces if len(f.verts) > 3]
    while stack:
        f = stack.pop()
        vs = list(f.verts)
        n = len(vs)
        if n <= 3:
            continue
        new = [i for i, v in enumerate(vs) if v.index >= first_new or v.index < 0]
        pair = None
        if n > 4:
            pair = next(((i, (i + 2) % n) for i in new if (i + 2) % n in new), None)
            if pair is None:
                i = new[0] if new else 0
                pair = (i, (i + 2) % n)
        else:
            sound = []
            for i, j in ((0, 2), (1, 3)):
                k, m = (i + 1) % 4, (j + 1) % 4
                if flat(vs[i].co, vs[k].co, vs[j].co) or flat(vs[j].co, vs[m].co, vs[i].co):
                    continue
                sound.append(((vs[i].co - vs[j].co).length, i, j))
            if not sound:
                raise SystemExit('landscape_ground: a quad without a sound diagonal')
            _, i, j = min(sound)
            pair = (i, j)
        other, _ = bmesh.utils.face_split(f, vs[pair[0]], vs[pair[1]])
        stack += [f, other]


def edit(ctx):
    me = ctx.mesh
    names = [gothic_name(m) for m in me.materials]
    ground = [first(MATERIAL_RULES, n) == 'ground' and not n.startswith('S:') for n in names]
    road = [g and 'PATH' in n for g, n in zip(ground, names)]

    old_normal = {}
    for loop, normal in zip(me.loops, me.corner_normals):
        old_normal.setdefault(loop.vertex_index, Vector(normal.vector))

    bm = bmesh.new()
    bm.from_mesh(me)
    vid = bm.verts.layers.float.get('_VERTEX')
    fid = bm.verts.layers.float.get('_FEATURE')
    kept = bm.verts.layers.float_vector.new('landscape_old_normal')
    for v in bm.verts:
        v[kept] = old_normal.get(v.index, Vector((0.0, 0.0, 1.0)))
    # A layer cannot tell the new vertices (a split gives the new vertex its ends' values); the index can: no
    # vertex is removed here, so the new ones come after the base's.
    base_count = len(bm.verts)
    base_co = [v.co.copy() for v in bm.verts]
    gothic0 = [to_gothic(v.co) for v in bm.verts]
    lo = (min(g.x for g in gothic0), min(g.z for g in gothic0))
    hi = (max(g.x for g in gothic0), max(g.z for g in gothic0))
    segments = waynet_segments(ctx, lo, hi)

    def is_ground(f):
        return ground[f.material_index]

    def edge_table():
        """Per welded edge: [faces]; an edge may be split when exactly two faces meet at it and both are ground."""
        table = {}
        for f in bm.faces:
            ks = [key(v.co) for v in f.verts]
            for i in range(len(ks)):
                a, b = ks[i], ks[(i + 1) % len(ks)]
                table.setdefault((a, b) if a <= b else (b, a), []).append(f)
        return table

    def edge_key(e):
        a, b = key(e.verts[0].co), key(e.verts[1].co)
        return (a, b) if a <= b else (b, a)

    faces_before = sum(1 for f in bm.faces if is_ground(f))
    for _ in range(PASSES):
        table = edge_table()
        split = []
        for e in bm.edges:
            faces = table.get(edge_key(e), ())
            if len(faces) != 2 or not all(is_ground(f) for f in faces):
                continue
            if e.calc_length() > STEP_CM:
                split.append(e)
        if not split:
            break
        before = len(bm.verts)
        bmesh.ops.subdivide_edges(bm, edges=split, cuts=1, use_grid_fill=False)
        bm.verts.index_update()
        cut_polygons(bm, before)
        # The twins of a seam are split by separate edges, and the two middles can differ in the last bit:
        # each place gets one position again, or the check finds the two halves open.
        bm.verts.index_update()
        for v, target in bmesh.ops.find_doubles(bm, verts=bm.verts[:], dist=WELD_CM)['targetmap'].items():
            if v.index >= base_count:
                v.co = target.co.copy()
            elif target.index >= base_count:
                target.co = v.co.copy()
    bm.verts.index_update()
    bm.verts.ensure_lookup_table()
    if any(bm.verts[i].co != base_co[i] for i in range(base_count)):
        raise SystemExit('landscape_ground: the base vertices are not the first ones any more')
    bm.faces.ensure_lookup_table()
    for v in bm.verts:
        if v.index >= base_count:
            if vid is not None:
                v[vid] = -1.0
            if fid is not None:
                v[fid] = -1.0

    # What is fixed: the ends of every welded edge that is open, shared by more than two faces, or touches a face
    # that is not ground. The relief fades towards those edges.
    table = edge_table()
    fixed, samples = set(), []
    for (a, b), faces in table.items():
        if len(faces) == 2 and all(is_ground(f) for f in faces):
            continue
        if not any(is_ground(f) for f in faces):
            continue                      # an edge among other faces only: nothing of the ground hangs on it
        fixed.update((a, b))
        va, vb = Vector(a), Vector(b)
        n = max(1, int((vb - va).length / 25.0))
        samples.extend(va.lerp(vb, i / n) for i in range(n + 1))
    # a vertex of any face that is not ground is fixed as well
    touched_ground = {}
    road_share = {}
    for f in bm.faces:
        for v in f.verts:
            k = key(v.co)
            if is_ground(f):
                touched_ground.setdefault(k, True)
                share = road_share.setdefault(k, [0, 0])
                share[0] += 1 if road[f.material_index] else 0
                share[1] += 1
            else:
                touched_ground[k] = False
    tree = KDTree(max(1, len(samples)))
    for i, co in enumerate(samples):
        tree.insert(co, i)
    if not samples:
        tree.insert(Vector((1e9, 1e9, 1e9)), 0)
    tree.balance()

    gothic = [to_gothic(v.co) for v in bm.verts]

    # The steepest ground face at each place, before anything moves.
    steepest = {}
    for f in bm.faces:
        if not is_ground(f):
            continue
        f.normal_update()
        slope = math.degrees(math.acos(max(-1.0, min(1.0, abs(f.normal.z)))))
        for v in f.verts:
            k = key(v.co)
            steepest[k] = max(steepest.get(k, 0.0), slope)

    # Per free place: how far it may move, and the tussocks there.
    limit, relief = {}, {}
    for v, g in zip(bm.verts, gothic):
        k = key(v.co)
        if k in limit:
            continue
        if k in fixed or not touched_ground.get(k, False):
            limit[k] = 0.0
            continue
        _, _, distance = tree.find(v.co)
        weight = smoothstep(distance / FADE_CM) * \
            (1.0 - smoothstep((steepest.get(k, 0.0) - SLOPE_FULL_DEG) / (SLOPE_NONE_DEG - SLOPE_FULL_DEG)))
        away = smoothstep((way_distance(g.x, g.z, segments) - WAY_NEAR_CM) / (WAY_FAR_CM - WAY_NEAR_CM))
        limit[k] = weight * (WAY_LIMIT_CM + (LIMIT_CM - WAY_LIMIT_CM) * away)
        soil = sum(amp * noise(g.x / wave, g.z / wave, SEED + i) for i, (wave, amp) in enumerate(SOIL))
        share = road_share.get(k, [0, 1])
        r = share[0] / share[1]
        relief[k] = weight * soil * (WAY_SHARE + (1.0 - WAY_SHARE) * away) * (1.0 - (1.0 - ROAD_SHARE) * r)

    # Rounded folds: the height of a free place goes towards the mean of its neighbours over the ground's edges,
    # never further from where it was than its limit.
    height0, around = {}, {}
    for f in bm.faces:
        if not is_ground(f):
            continue
        ks = [key(v.co) for v in f.verts]
        for v, k in zip(f.verts, ks):
            height0[k] = v.co.z
        for i in range(len(ks)):
            a, b = ks[i], ks[(i + 1) % len(ks)]
            around.setdefault(a, set()).add(b)
            around.setdefault(b, set()).add(a)
    height = dict(height0)
    free = [k for k, room in limit.items() if room > 0.0 and k in around]
    for _ in range(SMOOTH_PASSES):
        step = {}
        for k in free:
            mean = sum(height[n] for n in around[k]) / len(around[k])
            target = height[k] + SMOOTH_STEP * (mean - height[k])
            step[k] = max(height0[k] - limit[k], min(height0[k] + limit[k], target))
        height.update(step)
    moved = {}
    for k, room in limit.items():
        if room <= 0.0:
            moved[k] = 0.0
            continue
        total = height.get(k, 0.0) - height0.get(k, 0.0) + relief.get(k, 0.0)
        moved[k] = max(-room, min(room, total))
    old_keys = [key(v.co) for v in bm.verts]
    for v, k in zip(bm.verts, old_keys):
        v.co.z += moved[k]          # Blender z is Gothic's up

    # Smooth normals of the new ground, per position.
    smooth = {}
    for f in bm.faces:
        if not is_ground(f):
            continue
        f.normal_update()
        weighted = f.normal * f.calc_area()
        for v in f.verts:
            k = key(v.co)
            smooth[k] = smooth.get(k, Vector()) + weighted
    bm.verts.index_update()
    normal_of = []
    for v, k in zip(bm.verts, old_keys):
        if v.index < base_count and moved[k] == 0.0:
            normal_of.append(Vector(v[kept]))
            continue
        n = smooth.get(key(v.co))
        normal_of.append(n.normalized() if n is not None and n.length > 1e-9 else Vector(v[kept]))

    faces_after = sum(1 for f in bm.faces if is_ground(f))
    heights = [abs(h) for h in moved.values() if h != 0.0]
    bm.verts.layers.float_vector.remove(kept)
    bm.to_mesh(me)
    bm.free()
    me.normals_split_custom_set([normal_of[loop.vertex_index] for loop in me.loops])
    print(f'landscape_ground: {faces_before} -> {faces_after} ground triangles; {len(heights)} places moved, '
          f'mean {sum(heights) / max(1, len(heights)):.1f} cm, max {max(heights, default=0.0):.1f} cm; '
          f'{len(segments)} waynet edges near the chunk')
