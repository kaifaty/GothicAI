"""Real steps instead of the painted ramps of the stair from the harbour up to the barracks, as harbour_stairs.py did
for the stair under the upper-town path.

An edit of world_edit.py on the chunk NEWWORLD/0_0 (set stairskasern-1):

    python3 scripts/content/world/world_edit.py build harbour-stairs-kasern-newworld-0-0

By the waypoint NW_CITY_HABOUR_KASERN_07 the path from the harbour climbs 4.50 m between stone walls in two flights
of NW_HARBOUR_STAIRS_01 with a landing between them. Each flight is two triangles, one texture tile along the slope:
6 painted steps, a light tread band (42 % of the period) over a dark riser (58 %), 2.25 m rise over 3.5 m. The
lower flight is a trapezoid: its foot is cut askew by the yard, 2.45 m wide at the bottom and 4.50 m at the top.
The edit replaces each flight with 8 steps of 28.1 cm, the riser of the first stair (stairs-3: 26.7 cm), each nosing
with a 4 cm worn chamfer, the inner corners on the old ramp line, so the steps run into the side walls and the landing
without gaps; the sides follow the flight's own edges, so the trapezoid stays a trapezoid. Each step takes one painted
period of the texture: the tread its light band, the riser its stone face with the joints and the dark line at the
foot. The first versions gave the riser the light band, as the first stair does: in the wall's shadow its streaks
without joints read as smeared dark marble (reviewers, 2026-10-30). The new faces keep the ramp's normal, so the
engine lights them as it lit the ramp, and the risers lean back 25 degrees: the sun stands behind the stair, and
upright risers lie in the shadow of the step above - the lower flight came out a third darker than the ramp from the
yard, whatever their normal and texture band (trials, 2026-10-30). U across as on the ramp. Everything else in the
chunk is kept as it was imported.

The converted chunk is Gothic content (a derivative, user's decision 2026-10-24): it stays outside git.
"""
import math

import bmesh
from mathutils import Vector

from world_bpy import gothic_name, to_blender, to_gothic

PER_TILE = 6                # painted steps per texture tile along the slope
RISER = 320.0 / 12          # cm: the riser of the first stair (stairs-3, harbour_stairs.py), which this one keeps
# One painted period, measured on the texture (folded profile of the 6 periods, from the top of the period down the
# ramp): light tread band to 0.42, stone face of the riser to 0.75, the dark line of the riser's foot to 0.89, then
# the back of the next tread.
TREAD_BAND = (-0.14, 0.42)  # back of the tread .. nosing, in periods from the top of the step's period
RISER_BAND = (0.42, 0.86)   # top .. foot of the riser
RAKE = 25.0                 # degrees a riser leans back: the riser leans 11 cm, the tread in sight is 21-29 cm
NOSING = 4.0                # cm, worn chamfer on each nosing
NOSING_SHARE = 0.06         # the lower end of the light band goes on the chamfer
SHADE = 'ramp'              # normals of the new faces: 'faces' their own, 'ramp' the ramp's, lit as the ramp was
MATERIAL = 'NW_HARBOUR_STAIRS_01'
# The two flights in Gothic centimetres (x, z): by NW_CITY_HABOUR_KASERN_07.
BOX = ((1500.0, 2300.0), (4900.0, 5950.0))
PARAMETERS = {'material': MATERIAL, 'steps_per_tile': PER_TILE, 'riser_cm': RISER, 'tread_band': list(TREAD_BAND),
              'riser_band': list(RISER_BAND), 'rake_deg': RAKE, 'nosing_cm': NOSING, 'nosing_share': NOSING_SHARE, 'shade': SHADE,
              'box_cm': {'x': list(BOX[0]), 'z': list(BOX[1])}}


def edit(ctx):
    me = ctx.mesh
    slot = next(i for i, m in enumerate(me.materials) if gothic_name(m) == MATERIAL)
    uv_layer = me.uv_layers.active.name
    old_normals = [Vector(n.vector) for n in me.corner_normals]

    bm = bmesh.new()
    bm.from_mesh(me)
    bm.verts.ensure_lookup_table()
    bm.faces.ensure_lookup_table()
    uv = bm.loops.layers.uv[uv_layer]
    vid = bm.verts.layers.float.get('_VERTEX')
    fid = bm.verts.layers.float.get('_FEATURE')
    loop_index = bm.loops.layers.int.new('loop_index')
    i = 0
    for f in bm.faces:
        for lp in f.loops:
            lp[loop_index] = i
            i += 1

    ramp = []
    for f in bm.faces:
        if f.material_index != slot:
            continue
        c = to_gothic(f.calc_center_median())
        if BOX[0][0] < c.x < BOX[0][1] and BOX[1][0] < c.z < BOX[1][1]:
            ramp.append(f)
    # flights: triangles sharing an edge
    flights = []
    for f in ramp:
        mate = next((g for fl in flights for g in fl if len(set(f.verts) & set(g.verts)) == 2), None)
        if mate is None:
            flights.append([f])
        else:
            next(fl for fl in flights if mate in fl).append(f)
    if len(flights) != 2 or any(len(fl) != 2 for fl in flights):
        raise SystemExit(f'expected 2 flights of 2 triangles, found {[len(fl) for fl in flights]}')

    ctx.allow_open(ramp, 'outline of the painted ramps')
    new_faces = []
    ramp_normals = []           # per flight, the mean of the ramp's own corner normals
    up = Vector((0.0, 1.0, 0.0))
    for flight in flights:
        corners = {}
        for f in flight:
            for lp in f.loops:
                corners[lp.vert.index] = (lp.vert, to_gothic(lp.vert.co), Vector(lp[uv].uv), lp)
        if len(corners) != 4:
            raise SystemExit(f'expected 4 corners of a flight, found {len(corners)}')
        ys = sorted(corners.values(), key=lambda c: c[1].y)
        bottom, top = ys[:2], ys[2:]
        b0, b1 = sorted(bottom, key=lambda c: c[2].x)
        t0, t1 = sorted(top, key=lambda c: c[2].x)
        rise = t0[1].y - b0[1].y
        if abs((t1[1].y - b1[1].y) - rise) > 1.0:
            raise SystemExit('the two sides of a flight rise differently')
        flat = lambda v: Vector((v.x, 0.0, v.z))
        run_l, run_r = flat(t0[1] - b0[1]), flat(t1[1] - b1[1])
        v_bottom, v_top = 1.0 - b0[2].y, 1.0 - t0[2].y           # glTF v (Blender stores 1 - v)
        # as many steps as the first stair's riser gives; each takes one painted period of the texture, so
        # the light band stays on the tread although the flight has more steps than the texture paints
        steps = round(rise / RISER)
        period = (v_bottom - v_top) / round((v_bottom - v_top) * PER_TILE)
        print(f'flight: rise {rise:.1f} cm, run {run_l.length:.1f}/{run_r.length:.1f} cm, width '
              f'{(b1[1] - b0[1]).length:.1f} at the foot, {(t1[1] - t0[1]).length:.1f} at the top; {steps} steps of '
              f'{rise / steps:.2f} cm; u {b0[2].x:.3f}..{b1[2].x:.3f} at the foot, {t0[2].x:.3f}..{t1[2].x:.3f} at the top')
        reuse = {('b', 0): b0[0], ('b', 1): b1[0], ('t', 0): t0[0], ('t', 1): t1[0]}
        cache = {}

        def point(w, t, h):
            base = b0[1].lerp(b1[1], w)
            return base + run_l.lerp(run_r, w) * (t / steps) + up * (rise * h / steps)

        def u_at(w, t):
            return (b0[2].x + (t0[2].x - b0[2].x) * t / steps) * (1 - w) + (b1[2].x + (t1[2].x - b1[2].x) * t / steps) * w

        def vert(w, t, h):
            key = (w, round(t, 6), round(h, 6))
            if key in cache:
                return cache[key]
            if (t, h) == (0, 0) and reuse[('b', w)].is_valid:
                v = reuse[('b', w)]
            elif (t, h) == (steps, steps) and reuse[('t', w)].is_valid:
                v = reuse[('t', w)]
            else:
                v = bm.verts.new(to_blender(point(w, t, h)))
                if vid is not None:
                    v[vid] = -1.0
                if fid is not None:
                    v[fid] = -1.0
            cache[key] = v
            return v

        horizontal = run_l.lerp(run_r, .5)
        ramp_normals.append(sum((old_normals[lp[loop_index]] for f in flight for lp in f.loops), Vector()).normalized())
        shade = len(ramp_normals) - 1
        tread_normal = to_blender(up)
        g_riser = -horizontal.normalized()
        riser_normal = to_blender(g_riser)
        nosing_normal = to_blender((up + g_riser).normalized())

        def face(quad, uvs, normal, shade):
            f = bm.faces.new(quad)
            f.normal_update()
            if f.normal.dot(normal) < 0:
                f.normal_flip()
            f.material_index = slot
            for lp in f.loops:
                k = quad.index(lp.vert)
                lp[uv].uv = (uvs[k].x, 1.0 - uvs[k].y)
                lp[loop_index] = -1 - shade
            new_faces.append(f)

        gn = NOSING / (horizontal.length / steps)
        hn = NOSING / (rise / steps)
        rk = (rise / steps - NOSING) * math.tan(math.radians(RAKE)) / (horizontal.length / steps)   # in goings
        for s in range(1, steps + 1):
            v0 = v_bottom - (s % PER_TILE or PER_TILE) * period      # top of the step's painted period, from the foot
            v_back, v_nose = (v0 + b * period for b in TREAD_BAND)
            v_top, v_foot = (v0 + b * period for b in RISER_BAND)
            v_edge = v_nose - NOSING_SHARE * period
            t = s - 1
            uv4 = lambda ta, tb, va, vb: [Vector((u_at(0, ta), va)), Vector((u_at(1, ta), va)),
                                          Vector((u_at(1, tb), vb)), Vector((u_at(0, tb), vb))]
            # riser: upright from the back of the tread below, its stone face, the dark line at the foot
            face([vert(0, t, t), vert(1, t, t), vert(1, t + rk, s - hn), vert(0, t + rk, s - hn)],
                 uv4(t, t + rk, v_foot, v_top), riser_normal, shade)
            face([vert(0, t + rk, s - hn), vert(1, t + rk, s - hn), vert(1, t + rk + gn, s), vert(0, t + rk + gn, s)],
                 uv4(t + rk, t + rk + gn, v_top, v_edge), nosing_normal, shade)
            face([vert(0, t + rk + gn, s), vert(1, t + rk + gn, s), vert(1, s, s), vert(0, s, s)],
                 uv4(t + rk + gn, s, v_edge, v_back), tread_normal, shade)
        bmesh.ops.delete(bm, geom=[f for f in flight if f.is_valid], context='FACES_ONLY')

    ctx.allow_open(new_faces, 'the sides of the steps run into the side walls and the landing')
    bm.to_mesh(me)
    bm.free()
    me.validate()
    me.update()

    li = me.attributes['loop_index'].data
    poly_of_loop = [0] * len(me.loops)
    for p in me.polygons:
        for k in p.loop_indices:
            poly_of_loop[k] = p.index
    normals = []
    for k in range(len(me.loops)):
        old = li[k].value
        if old >= 0:
            normals.append(old_normals[old])
        elif SHADE == 'ramp':
            normals.append(ramp_normals[-1 - old])
        else:
            normals.append(Vector(me.polygons[poly_of_loop[k]].normal))
    me.normals_split_custom_set(normals)
    me.attributes.remove(me.attributes['loop_index'])
    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    print(f'chunk: {len(me.polygons)} faces, {tris} triangles after the stairs')
