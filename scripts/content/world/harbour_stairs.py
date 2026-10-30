"""Real steps instead of the painted ramp of the harbour stairs (docs/asset-migration.md, step П2).

An edit of world_edit.py on the chunk NEWWORLD/1_-1 (set stairs-3):

    python3 scripts/content/world/world_edit.py build harbour-stairs-newworld-1-1

The ramp NW_HARBOUR_STAIRS_01 below the upper-town path is two triangles with
12 steps painted on them: one texture tile along the slope holds 6 steps, a
light tread band (42 % of the period) over a dark riser (58 %). The edit
replaces the two triangles with 12 treads and risers in the original's
proportions, each nosing with a 4 cm worn chamfer, (3.20 m rise, 4.00 m run: 26.7 cm riser, 33.3 cm going), the inner
corners on the old ramp line so the steps run into the side walls without gaps.
Treads and risers both get the light band: the dark band is painted shadow, which the
engine now casts itself on the real risers. U across the stair as on the ramp. Everything else in the chunk is kept as it was imported.

The converted chunk is Gothic content (a derivative, user's decision
2026-10-24): it stays outside git.
"""
import bmesh
from mathutils import Vector

from world_bpy import gothic_name, to_blender, to_gothic

STEPS = 12
TREAD_SHARE = 0.42          # light band of one period, measured on the texture
NOSING = 4.0                # cm, worn chamfer on each nosing: it catches the light seen from below
NOSING_SHARE = 0.10         # the lower end of the light band goes on the chamfer
MATERIAL = 'NW_HARBOUR_STAIRS_01'
# The ramp in Gothic centimetres (x, z): the bench camera NW_HARBOUR_STAIRS_01 looks at it.
RAMP_BOX = ((8300.0, 8850.0), (-5150.0, -4550.0))
PARAMETERS = {'material': MATERIAL, 'steps': STEPS, 'tread_share': TREAD_SHARE, 'nosing_cm': NOSING,
              'nosing_share': NOSING_SHARE, 'ramp_box_cm': {'x': list(RAMP_BOX[0]), 'z': list(RAMP_BOX[1])}}


def edit(ctx):
    me = ctx.mesh
    slot = next(i for i, m in enumerate(me.materials) if gothic_name(m) == MATERIAL)

    uv_layer = me.uv_layers.active.name
    color_name = me.color_attributes.active_color.name if me.color_attributes.active_color else None
    old_normals = [Vector(n.vector) for n in me.corner_normals]

    bm = bmesh.new()
    bm.from_mesh(me)
    bm.verts.ensure_lookup_table()
    bm.faces.ensure_lookup_table()
    uv = bm.loops.layers.uv[uv_layer]
    vid = bm.verts.layers.float.get('_VERTEX')
    fid = bm.verts.layers.float.get('_FEATURE')
    color_point = bm.verts.layers.color.get(color_name) if color_name else None
    color_corner = bm.loops.layers.color.get(color_name) if color_name else None
    # Keep each old loop's normal across the edit: mark loops with their index.
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
        if RAMP_BOX[0][0] < c.x < RAMP_BOX[0][1] and RAMP_BOX[1][0] < c.z < RAMP_BOX[1][1]:
            ramp.append(f)
    if len(ramp) != 2:
        raise SystemExit(f'expected the 2 ramp triangles, found {len(ramp)}')

    corners = {}
    for f in ramp:
        for lp in f.loops:
            corners[lp.vert.index] = (lp.vert, to_gothic(lp.vert.co), Vector(lp[uv].uv), lp)
    if len(corners) != 4:
        raise SystemExit(f'expected 4 ramp corners, found {len(corners)}')
    ys = sorted(corners.values(), key=lambda c: c[1].y)
    bottom, top = ys[:2], ys[2:]
    # Left side = the smaller U.
    b0, b1 = sorted(bottom, key=lambda c: c[2].x)
    t0, t1 = sorted(top, key=lambda c: c[2].x)
    width = b1[1] - b0[1]
    run = t0[1] - b0[1]
    rise = run.y
    horizontal = Vector((run.x, 0.0, run.z))
    u0, u1 = b0[2].x, b1[2].x
    # Work in glTF v (Blender stores 1 - v).
    v_bottom, v_top = 1.0 - b0[2].y, 1.0 - t0[2].y
    period = (v_bottom - v_top) / STEPS
    if color_corner is not None:
        ramp_color = tuple(b0[3][color_corner])
    elif color_point is not None:
        ramp_color = tuple(b0[0][color_point])
    else:
        ramp_color = None
    print(f'ramp: width {width.length:.1f} cm, rise {rise:.1f} cm, run {horizontal.length:.1f} cm; '
          f'step {rise / STEPS:.2f} x {horizontal.length / STEPS:.2f} cm; u {u0:.3f}..{u1:.3f}, '
          f'v {v_top:.3f}..{v_bottom:.3f}')

    # the ramp's outline opens where the walls met it: the steps meet the walls along the same line
    ctx.allow_open(ramp, 'outline of the painted ramp')
    bmesh.ops.delete(bm, geom=ramp, context='FACES_ONLY')
    reuse = {k: c[0] for k, c in (('b0', b0), ('b1', b1), ('t0', t0), ('t1', t1)) if c[0].is_valid}

    def point(w, t, h):
        return b0[1] + width * w + horizontal * t + Vector((0.0, h, 0.0))

    cache = {}
    gn = NOSING / (horizontal.length / STEPS)    # chamfer depth as a share of one going

    def vert(w, t, h):
        # A stair corner: side (0 left, 1 right), run in steps, height in steps (both may be fractional).
        key = (w, round(t, 6), round(h, 6))
        if key in cache:
            return cache[key]
        if (t, h) == (0, 0) and ('b0' if w == 0 else 'b1') in reuse:
            v = reuse['b0' if w == 0 else 'b1']
        elif (t, h) == (STEPS, STEPS) and ('t0' if w == 0 else 't1') in reuse:
            v = reuse['t0' if w == 0 else 't1']
        else:
            v = bm.verts.new(to_blender(point(w, t / STEPS, rise * h / STEPS)))
            if vid is not None:
                v[vid] = -1.0
            if fid is not None:
                v[fid] = -1.0
            if color_point is not None and ramp_color is not None:
                v[color_point] = ramp_color
        cache[key] = v
        return v

    tread_normal = to_blender(Vector((0.0, 1.0, 0.0)))
    g_riser = -horizontal.normalized()
    riser_normal = to_blender(g_riser)
    nosing_normal = to_blender((Vector((0.0, 1.0, 0.0)) + g_riser).normalized())   # 45 degrees: as deep as high
    new_faces = []

    def face(quad, uvs, normal):
        f = bm.faces.new(quad)
        f.normal_update()
        if f.normal.dot(normal) < 0:
            f.normal_flip()
        f.material_index = slot
        for lp in f.loops:
            k = quad.index(lp.vert)
            lp[uv].uv = (uvs[k].x, 1.0 - uvs[k].y)
            lp[loop_index] = -1
            if color_corner is not None and ramp_color is not None:
                lp[color_corner] = ramp_color
        new_faces.append((f, normal))

    hn = NOSING / (rise / STEPS)            # chamfer height as a share of one step
    for s in range(1, STEPS + 1):
        j = STEPS - s                      # period counted from the top of the ramp
        v_back = v_top + j * period        # up-ramp edge of the tread
        v_nose = v_back + TREAD_SHARE * period
        v_edge = v_nose - NOSING_SHARE * period
        t0, h0 = s - 1, s - 1
        # Riser s: at run t0, from height h0 to just below the chamfer.
        a, b = vert(0, t0, h0), vert(1, t0, h0)
        c, d = vert(1, t0, s - hn), vert(0, t0, s - hn)
        # The dark band is painted shadow: with a real riser the engine shades it, so the riser
        # takes the stone of the light band (its upper edge meets the chamfer).
        face([a, b, c, d], [Vector((u0, v_back)), Vector((u1, v_back)), Vector((u1, v_edge)), Vector((u0, v_edge))],
             riser_normal)
        # The worn chamfer of the nosing.
        a, b = vert(0, t0, s - hn), vert(1, t0, s - hn)
        c, d = vert(1, t0 + gn, s), vert(0, t0 + gn, s)
        face([a, b, c, d], [Vector((u0, v_nose)), Vector((u1, v_nose)), Vector((u1, v_edge)), Vector((u0, v_edge))],
             nosing_normal)
        # Tread s: at height s, from behind the chamfer to the next riser.
        a, b = vert(0, t0 + gn, s), vert(1, t0 + gn, s)
        c, d = vert(1, s, s), vert(0, s, s)
        face([a, b, c, d], [Vector((u0, v_edge)), Vector((u1, v_edge)), Vector((u1, v_back)), Vector((u0, v_back))],
             tread_normal)

    ctx.allow_open([f for f, _ in new_faces], 'the sides of the steps run into the side walls')
    bm.to_mesh(me)
    bm.free()
    me.validate()
    me.update()

    # Old loops keep their imported normal; new faces are flat.
    li = me.attributes['loop_index'].data
    normals = []
    poly_of_loop = [0] * len(me.loops)
    for p in me.polygons:
        for k in p.loop_indices:
            poly_of_loop[k] = p.index
    flat = {}
    for p in me.polygons:
        if li[p.loop_start].value == -1:
            flat[p.index] = Vector(p.normal)
    for k in range(len(me.loops)):
        old = li[k].value
        if old >= 0:
            normals.append(old_normals[old])
        else:
            normals.append(flat[poly_of_loop[k]])
    me.normals_split_custom_set(normals)
    me.attributes.remove(me.attributes['loop_index'])

    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    print(f'chunk: {len(me.polygons)} faces, {tris} triangles after the stairs')
