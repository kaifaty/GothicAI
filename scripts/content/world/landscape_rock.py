"""Rock of the landscape pilot as a refined mesh: variant A of the rock probe (docs/landscape-plan.md, step В2;
method note reports/content/2026-11-landscape-pilot/b2-geometry.md, way 4).

An edit of world_edit.py build-region. It takes the natural rock (class `rock` of world_classes.py, without the room
copies `S:…` of caves) within RADIUS_CM of a waypoint, and

- splits its edges until none is longer than STEP_CM (the base's rock faces are 4-12 m);
- rounds the ridges between the base's flat faces: ROUND_PASSES of a Laplacian on the positions;
- pushes the surface in and out along its normal as bedded sandstone weathers (the reference photos of the pilot
  note): level beds 1.5-3.8 m thick, each a rounded cushion with its own overhang, recessed bedding joints between
  them, two families of wide vertical joints, and a foot undercut where the rock stands on the ground. Tops (normals
  near up) take only the slow unevenness.

The layout of the texture stays the base's: new vertices take interpolated UVs, so the stretch of the texture on
steep faces stays as it is (variant B and the materials of В3 deal with it).

Rock one can stand on (not steeper than WALK_DEG) moves no further than WALK_LIMIT_CM, and the beds and joints are
for faces steeper than WALL_FROM_DEG: the pilot's tolerance on walkable ground is 30 cm.

What stays where it was: every place the chosen rock shares with anything else (ground, vegetation, man-made, rock
outside the radius or more than BELOW_CM under the waypoint, the region's outer border); the relief fades to nothing
within FADE_CM of such a place. Near the waynet nothing moves further than WAY_LIMIT_CM.

The base has rows of vertices at the same places with other _VERTEX ids (meshes put together without welding), which
the region's weld keeps apart. Everything here is decided per place (the exact position, as the check welds): the
twins of such a row are split alike (new twins are snapped together within WELD_CM) and moved alike.

A face may not turn further than TURN_DEG from where it faced: where the rounding or the relief folds the surface
(rock sheets that meet at an angle), the places go halfway back until it does not.

Normals: a corner of a moved or new rock vertex takes the smooth normal of the rock faces around its place that face
its own face's way; every other corner keeps its base normal. New vertices get _VERTEX -1 (the build bakes their light from the base around them).

The numbers are estimates by eye from the photos, not measurements; a clip corrects them.
"""
import bisect
import json
import math
from pathlib import Path

import bmesh
from mathutils import Vector
from mathutils.kdtree import KDTree

from landscape_ground import _hash, noise, smoothstep, way_distance, waynet_segments
from world_bpy import gothic_name, to_blender, to_gothic
from world_classes import MATERIAL_RULES, first

WAYPOINT = 'NW_FARM2_TO_TAVERN_02'
RADIUS_CM = 3500.0
BELOW_CM = 2000.0        # rock deeper under the waypoint than this (the cave below) is left
STEP_CM = 80.0           # no rock edge longer than this
PASSES = 8
WELD_CM = 0.02           # twins after a split: a float step is 0.004 cm at 400 m from the origin
ROUND_PASSES = 10
ROUND_STEP = 0.5
FADE_CM = 120.0          # the relief reaches full depth this far from a vertex that stays
BED_MIN_CM, BED_MAX_CM = 150.0, 380.0
BED_BULGE_CM = (20.0, 36.0)      # how far the middle of a bed stands out of its joints
BED_OFFSET_CM = (-10.0, 20.0)    # a bed's own overhang
BED_WARP_CM, BED_WARP_LEN_CM = 40.0, 900.0   # the beds are level within this
JOINT_SPACING_CM = (520.0, 640.0)
JOINT_ANGLE_DEG = 25.0
JOINT_HALF_CM, JOINT_DEPTH_CM = 70.0, 22.0
JOINT_SHARE = 0.6        # of the vertical joints show
SLOW_CM, SLOW_LEN_CM = 12.0, 700.0
UNDER_REACH_CM, UNDER_DEPTH_CM = 160.0, 28.0
LIMIT_CM = 75.0          # nothing moves further than this
WALK_LIMIT_CM = 25.0     # ... and rock one can stand on (not steeper than WALK_DEG) no further than this: the
WALK_DEG = 53.0          # pilot allows 30 cm on walkable ground, walking stops above 50 degrees
WALL_FROM_DEG, WALL_FULL_DEG = 41.0, 66.0   # the beds and joints show on faces steeper than this
TURN_DEG = 70.0          # a face may not turn further than this from where it faced: no folds
WAY_NEAR_CM, WAY_FAR_CM, WAY_LIMIT_CM = 50.0, 200.0, 4.0
SEED = 20261113
INPUTS = [Path(__file__).with_name('landscape_ground.py')]
PARAMETERS = {'waypoint': WAYPOINT, 'radius_cm': RADIUS_CM, 'below_cm': BELOW_CM, 'step_cm': STEP_CM, 'round_passes': ROUND_PASSES,
              'round_step': ROUND_STEP, 'fade_cm': FADE_CM, 'bed_cm': [BED_MIN_CM, BED_MAX_CM],
              'bed_bulge_cm': list(BED_BULGE_CM), 'bed_offset_cm': list(BED_OFFSET_CM),
              'bed_warp_cm': [BED_WARP_CM, BED_WARP_LEN_CM], 'joint_spacing_cm': list(JOINT_SPACING_CM),
              'joint_angle_deg': JOINT_ANGLE_DEG, 'joint_cm': [JOINT_HALF_CM, JOINT_DEPTH_CM],
              'joint_share': JOINT_SHARE, 'slow_cm': [SLOW_CM, SLOW_LEN_CM],
              'undercut_cm': [UNDER_REACH_CM, UNDER_DEPTH_CM], 'limit_cm': LIMIT_CM, 'walk_limit_cm': WALK_LIMIT_CM, 'walk_deg': WALK_DEG,
              'wall_deg': [WALL_FROM_DEG, WALL_FULL_DEG], 'turn_deg': TURN_DEG,
              'way_cm': [WAY_NEAR_CM, WAY_FAR_CM, WAY_LIMIT_CM], 'seed': SEED}


def waypoint(ctx, name):
    net = json.loads((ctx.base_chunks.parent / 'waynet.json').read_text())['waynet']
    for p in net['points']:
        if p['name'] == name:
            return to_blender(Vector(p['position']))
    raise SystemExit(f'landscape_rock: no waypoint {name}')


def unit(i, k):
    """0..1, the same for the same pair."""
    return _hash(i, k, SEED) * 0.5 + 0.5


def bed_levels(low, high):
    levels, i = [low - 400.0], 0
    while levels[-1] < high + 400.0:
        levels.append(levels[-1] + BED_MIN_CM + (BED_MAX_CM - BED_MIN_CM) * unit(i, 7))
        i += 1
    return levels


def bedding(co, levels):
    """How far a wall stands out at a place, cm: its bed's cushion and overhang."""
    z = co.z + BED_WARP_CM * noise(co.x / BED_WARP_LEN_CM, co.y / BED_WARP_LEN_CM, SEED + 1)
    i = min(len(levels) - 2, max(0, bisect.bisect_right(levels, z) - 1))
    t = min(1.0, max(0.0, (z - levels[i]) / (levels[i + 1] - levels[i])))
    cushion = (4.0 * t * (1.0 - t)) ** 0.7
    bulge = BED_BULGE_CM[0] + (BED_BULGE_CM[1] - BED_BULGE_CM[0]) * unit(i, 11)
    offset = BED_OFFSET_CM[0] + (BED_OFFSET_CM[1] - BED_OFFSET_CM[0]) * unit(i, 13)
    return offset + bulge * (cushion - 0.6)


def joints(co):
    """The recess of the nearest vertical joint, cm (0 or negative)."""
    a = math.radians(JOINT_ANGLE_DEG)
    bend = 60.0 * noise(co.x / 500.0, co.y / 500.0, SEED + 2)
    along = (co.x * math.cos(a) + co.y * math.sin(a) + bend, -co.x * math.sin(a) + co.y * math.cos(a) - bend)
    deepest = 0.0
    for family, (value, spacing) in enumerate(zip(along, JOINT_SPACING_CM)):
        cell = math.floor(value / spacing + 0.5)
        if unit(cell, 17 + family) > JOINT_SHARE:
            continue
        distance = abs(value - cell * spacing)
        deepest = min(deepest, -JOINT_DEPTH_CM * (1.0 - smoothstep(distance / JOINT_HALF_CM)))
    return deepest


def key(co):
    """A place, exactly as the check welds it (chunk_glb.position_keys)."""
    return (co.x + 0.0, co.y + 0.0, co.z + 0.0)


def edit(ctx):
    me = ctx.mesh
    names = [gothic_name(m) for m in me.materials]
    rock = [first(MATERIAL_RULES, n) == 'rock' and not n.startswith('S:') for n in names]
    centre = waypoint(ctx, WAYPOINT)
    base_normals = [Vector(n.vector) for n in me.corner_normals]

    bm = bmesh.new()
    bm.from_mesh(me)
    border = ctx.border_layer(bm)
    old = bm.loops.layers.float_vector.new('rock_old_normal')
    k = 0
    for f in bm.faces:
        for loop in f.loops:
            loop[old] = base_normals[k]
            k += 1
    chosen = bm.faces.layers.int.new('rock_chosen')
    before_faces = 0
    for f in bm.faces:
        c = f.calc_center_median()
        f[chosen] = int(rock[f.material_index] and math.hypot(c.x - centre.x, c.y - centre.y) <= RADIUS_CM and
                        c.z >= centre.z - BELOW_CM and not any(v[border] for v in f.verts))
        before_faces += f[chosen]
    if not before_faces:
        raise SystemExit(f'landscape_rock: no rock within {RADIUS_CM:.0f} cm of {WAYPOINT}')
    base_count = len(bm.verts)

    for _ in range(PASSES):
        cut = [e for e in bm.edges if e.calc_length() > STEP_CM and any(f[chosen] for f in e.link_faces)]
        if not cut:
            break
        before = len(bm.verts)          # a fresh bmesh puts the vertices it makes after its own
        bmesh.ops.subdivide_edges(bm, edges=cut, cuts=1, use_grid_fill=True)
        made = list(bm.verts)[before:]
        ctx.mark_new(bm, made)
        # the middles of twin edges differ in the last bit: one place again, the vertices stay apart
        for v, twin in bmesh.ops.find_doubles(bm, verts=made, dist=WELD_CM)['targetmap'].items():
            v.co = twin.co
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])

    bm.verts.index_update()
    place_of = [key(v.co) for v in bm.verts]
    members = {}
    for v in bm.verts:
        members.setdefault(place_of[v.index], []).append(v)
    free = {k for k, vs in members.items()
            if all(v.link_faces and all(f[chosen] for f in v.link_faces) for v in vs)}
    stay = [k for k, vs in members.items() if k not in free and any(f[chosen] for v in vs for f in v.link_faces)]
    tree = KDTree(len(stay))
    for n, k in enumerate(stay):
        tree.insert(Vector(k), n)
    tree.balance()
    around = {k: {place_of[e.other_vert(v).index] for v in members[k] for e in v.link_edges} - {k} for k in free}
    gothic = {k: to_gothic(Vector(k)) for k in free}
    segments = waynet_segments(ctx, (min(g.x for g in gothic.values()), min(g.z for g in gothic.values())),
                               (max(g.x for g in gothic.values()), max(g.z for g in gothic.values()))) if free else []
    nearest, fade, limit = {}, {}, {}
    for k in free:
        place, _, distance = tree.find(Vector(k))
        nearest[k] = (place, distance)
        fade[k] = smoothstep(distance / FADE_CM)
        g = gothic[k]
        away = smoothstep((way_distance(g.x, g.z, segments) - WAY_NEAR_CM) / (WAY_FAR_CM - WAY_NEAR_CM))
        limit[k] = WAY_LIMIT_CM + (LIMIT_CM - WAY_LIMIT_CM) * away

    def place_normals():
        bm.normal_update()
        out = {}
        for k in free:
            total = Vector()
            for v in members[k]:
                for f in v.link_faces:
                    total += f.normal * f.calc_area()
            out[k] = total.normalized() if total.length > 1e-9 else Vector()
        return out

    def put(at):
        for k, co in at.items():
            for v in members[k]:
                v.co = co

    bm.normal_update()
    faced = {f: f.normal.copy() for f in bm.faces if f[chosen]}
    walk = math.cos(math.radians(WALK_DEG))
    for k, n in place_normals().items():
        if n.z >= walk:
            limit[k] = min(limit[k], WALK_LIMIT_CM)

    # round the ridges of the base's flat faces
    at = {k: Vector(k) for k in free}
    for _ in range(ROUND_PASSES):
        step = {}
        for k in free:
            if not around[k]:
                continue
            mean = sum((at.get(n) or Vector(n) for n in around[k]), Vector()) / len(around[k])
            delta = at[k].lerp(mean, ROUND_STEP * fade[k]) - Vector(k)
            if delta.length > limit[k]:
                delta *= limit[k] / delta.length
            step[k] = Vector(k) + delta
        at.update(step)
    put(at)
    normal = place_normals()

    # bedded sandstone: cushions of the beds, bedding and vertical joints, the undercut foot
    wall_from, wall_full = (math.cos(math.radians(a)) for a in (WALL_FROM_DEG, WALL_FULL_DEG))
    heights = [k[2] for k in free]
    levels = bed_levels(min(heights), max(heights))
    moved = []
    for k in free:
        co, n = at[k], normal[k]
        wall = smoothstep((wall_from - n.z) / (wall_from - wall_full))
        depth = wall * (bedding(co, levels) + joints(co))
        depth += SLOW_CM * noise((co.x + co.z) / SLOW_LEN_CM, (co.y - co.z) / SLOW_LEN_CM, SEED + 3)
        place, distance = nearest[k]
        if place.z < k[2] - 10.0 and distance < UNDER_REACH_CM:
            depth -= wall * UNDER_DEPTH_CM * math.sin(math.pi * distance / UNDER_REACH_CM)
        delta = co + n * (depth * fade[k]) - Vector(k)
        if delta.length > limit[k]:
            delta *= limit[k] / delta.length
        at[k] = Vector(k) + delta
    put(at)
    # where the rounding and the relief folded the surface (a face turned over), the places go halfway back
    turn, folded = math.cos(math.radians(TURN_DEG)), 0
    for _ in range(10):
        bm.normal_update()
        back = {place_of[v.index] for f, n in faced.items() if n.length > 0.5 and f.normal.dot(n) < turn
                for v in f.verts} & free
        if not back:
            break
        folded = max(folded, len(back))
        for k in back:
            at[k] = Vector(k).lerp(at[k], 0.5)
        put(at)
    moved = [(at[k] - Vector(k)).length for k in free]
    bm.normal_update()

    # a corner's normal: the faces around its place that face its own face's way (two sheets back to back, or
    # meeting at more than a right angle, do not share normals)
    around_faces = {k: [(f.normal.copy(), f.calc_area()) for v in members[k] for f in v.link_faces] for k in free}
    normals = []
    for f in bm.faces:
        for loop in f.loops:
            k = place_of[loop.vert.index]
            if f[chosen] and k in free:
                total = sum((n * a for n, a in around_faces[k] if n.dot(f.normal) > 0.0), Vector())
                normals.append(total.normalized() if total.length > 1e-9 else f.normal.copy())
            else:
                n = Vector(loop[old])
                normals.append(n.normalized() if n.length > 1e-9 else f.normal.copy())
    after_faces = sum(1 for f in bm.faces if f[chosen])
    new = len(bm.verts) - base_count
    bm.loops.layers.float_vector.remove(old)
    bm.faces.layers.int.remove(chosen)
    bm.to_mesh(me)
    bm.free()
    me.normals_split_custom_set(normals)
    print(f'landscape_rock: {before_faces} -> {after_faces} rock triangles within {RADIUS_CM / 100:.0f} m of '
          f'{WAYPOINT}; {new} new vertices, {len(moved)} places moved, mean '
          f'{sum(moved) / max(1, len(moved)):.1f} cm, max {max(moved, default=0.0):.1f} cm; {len(levels) - 1} beds; {folded} places taken back from folds')
