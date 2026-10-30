"""Real holes in the shingle roofs NW_CITY_ROOF_01 of a block of the world mesh (pilot: harbour hut B037).

An edit of world_edit.py on the chunk NEWWORLD/0_-1 (set roofholes-2):

    python3 scripts/content/world/world_edit.py build harbour-roof-holes-newworld-0-1 \
        --materials <content dir>/materials/materials.json

The roof texture paints five holes per square where shingles fell out, with the dark attic, a lath and, in
the big one, a rafter (data/content/world/roof01-holes.json, read from our roofs-8 bake by roof_holes_layout.py). The roof itself
is two skins: the shingles NW_CITY_ROOF_01 outside and the boards *_ROOF_INSIDE_01 36-51 cm under them,
seen from inside the hut. For every hole the texture puts on a roof panel in the box, the edit cuts the
hole through both skins at the painted place (the painted black goes with the cut), lines its sides with
dark wood across the roof's thickness, and lays the lath under the shingles and the rafter under the lath,
so the hole shows depth from the street and the sky from inside. U and V of a hole map onto its panel by
the panel's own texture mapping. Everything else in the chunk stays as it was imported. The materials
OWN_ROOF_HOLE_SHADE and OWN_ROOF_HOLE_WALL come from roof_hole_shade.py.

The converted chunk is Gothic content (a derivative, user's decision 2026-10-24): it stays outside git.
"""
import json
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.geometry import tessellate_polygon

from world_bpy import gothic_name, to_gothic

ROOF = 'NW_CITY_ROOF_01'
INSIDE = 'ROOF_INSIDE_01'      # the inside skin's material ends with this (sector prefixes differ per hut)
WOOD = 'NW_CITY_WOODBEAM_02'   # sides of the hole, lath and rafter: the roof's own timber
BOX = (2570.0, 4930.0, -4490.0, -1990.0)   # block B037 in Gothic cm (x0, x1, z0, z1): census of the harbour
LATH = (10.0, 4.0)             # cm: width down the slope, depth (shingle_bake.py LATH)
RAFTER = (16.0, 20.0)          # cm: width across, depth (shingle_bake.py RAFTER)
REACH = 12.0                   # cm the lath and rafter run on under the shingles past the hole
SINK = 2.0                     # cm under the shingles: a panel is two triangles a little out of one plane
OVER = 5.0                     # cm the cutter stands out of each skin
SEAM = .8                      # cm the sides of a hole reach past its cut edge
MIN_SHARE = .3                 # a hole clipped by its panel's edge to less than this share is left painted
WOOD_UV = 150.0                # cm of wood per texture repeat along the grain
SHADE = 'OWN_ROOF_HOLE_SHADE'  # multiply face at the bottom of a hole, facing out (roof_hole_shade.py)
WALL = 'OWN_ROOF_HOLE_WALL'    # sides of a hole: the roof's timber darkened, the cavity under the shingles
HOLES = Path(__file__).resolve().parents[3] / 'data/content/world/roof01-holes.json'
INPUTS = [HOLES]
PARAMETERS = {'box_cm': list(BOX), 'lath_cm': list(LATH), 'rafter_cm': list(RAFTER), 'reach_cm': REACH,
              'shade': SHADE, 'wall': WALL}


def area2(poly):
    return sum(a.x * b.y - b.x * a.y for a, b in zip(poly, poly[1:] + poly[:1])) / 2


def clip(poly, u0, u1, v0, v1):
    """Sutherland-Hodgman against an axis-aligned box in UV."""
    def side(pts, inside, cut):
        out = []
        for a, b in zip(pts, pts[1:] + pts[:1]):
            if inside(b):
                if not inside(a):
                    out.append(cut(a, b))
                out.append(b)
            elif inside(a):
                out.append(cut(a, b))
        return out

    def at_u(u):
        return lambda a, b: a.lerp(b, (u - a.x) / (b.x - a.x))

    def at_v(v):
        return lambda a, b: a.lerp(b, (v - a.y) / (b.y - a.y))
    for inside, cut in ((lambda p: p.x >= u0, at_u(u0)), (lambda p: p.x <= u1, at_u(u1)),
                        (lambda p: p.y >= v0, at_v(v0)), (lambda p: p.y <= v1, at_v(v1))):
        poly = side(poly, inside, cut)
        if not poly:
            return []
    return poly


class Panel:
    """A roof triangle with its texture mapping: P(u, v) = p0 + du (u - u0) + dv (v - v0), Blender UV."""

    def __init__(self, face, uv):
        lp = face.loops
        self.face = face
        self.p0, self.t0 = lp[0].vert.co.copy(), lp[0][uv].uv.copy()
        e1, e2 = lp[1].vert.co - self.p0, lp[2].vert.co - self.p0
        t1, t2 = lp[1][uv].uv - self.t0, lp[2][uv].uv - self.t0
        det = t1.x * t2.y - t1.y * t2.x
        self.du = (e1 * t2.y - e2 * t1.y) / det
        self.dv = (e2 * t1.x - e1 * t2.x) / det
        self.uvs = [l[uv].uv.copy() for l in lp]
        self.normal = face.normal.copy()

        self.mates = []

    def affine(self, t):
        return self.p0 + self.du * (t.x - self.t0.x) + self.dv * (t.y - self.t0.y)

    def inside(self, t):
        """The smallest barycentric coordinate of t: >= 0 inside the triangle."""
        a, b, c = self.uvs
        d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y)
        l1 = ((b.y - c.y) * (t.x - c.x) + (c.x - b.x) * (t.y - c.y)) / d
        l2 = ((c.y - a.y) * (t.x - c.x) + (a.x - c.x) * (t.y - c.y)) / d
        return min(l1, l2, 1 - l1 - l2)

    def contains(self, t):
        return self.inside(t) >= 0

    def point(self, t):
        """Where UV t lies on the panel: through the triangle of the panel that holds it, as the texture does."""
        return max([self] + self.mates, key=lambda q: q.inside(t)).affine(t)


def edit(ctx, box=BOX):
    holes = json.loads(HOLES.read_text())['holes']
    obj = ctx.obj
    me = ctx.mesh
    names = [gothic_name(m) for m in me.materials]
    roof_slot = names.index(ROOF)
    wood_slot = names.index(WOOD)
    # the shade: the wood's record with its own name and texture, multiply blend, no collision, no source index
    def own_material(name, **fields):
        m = bpy.data.materials.new(name)
        record = me.materials[wood_slot]['gothic'].to_dict()
        record.pop('index', None)
        record.update(name=name, texture=name + '.TGA', **fields)
        m['gothic'] = record
        me.materials.append(m)
        return len(me.materials) - 1
    shade_slot = own_material(SHADE, alpha_func=5, disable_collision=True, color=[128, 128, 128, 255])
    wall_slot = own_material(WALL)
    uv_name = me.uv_layers.active.name
    color_name = me.color_attributes.active_color.name if me.color_attributes.active_color else None
    old_normals = [Vector(n.vector) for n in me.corner_normals]

    def in_box(f):
        c = to_gothic(f.calc_center_median())
        return box[0] <= c.x <= box[1] and box[2] <= c.z <= box[3]

    # --- the cutters, from the outside skin's panels
    bm = bmesh.new()
    bm.from_mesh(me)
    uv = bm.loops.layers.uv[uv_name]
    outer = [f for f in bm.faces if f.material_index == roof_slot and in_box(f)]
    inner = [f for f in bm.faces if names[f.material_index].endswith(INSIDE) and in_box(f)]
    inner_tree = BVHTree.FromPolygons([v.co for v in bm.verts], [[v.index for v in f.verts] for f in inner]) if inner else None
    panels = [Panel(f, uv) for f in outer]
    pos_key = lambda co: tuple(round(x, 3) for x in co)
    by_vert = {}
    for p in panels:
        for v in p.face.verts:
            by_vert.setdefault(pos_key(v.co), []).append(p)

    cutters = []    # (outer polygon, inner polygon, panel, hole)
    for p in panels:
        # the panel's UV rectangle: this triangle and the one sharing its long edge
        mates = {q for v in p.face.verts for q in by_vert[pos_key(v.co)]
                 if q is not p and sum(pos_key(w.co) in {pos_key(x.co) for x in p.face.verts} for w in q.face.verts) == 2}
        p.mates = list(mates)
        uvs = p.uvs + [t for q in mates for t in q.uvs]
        u0, u1 = min(t.x for t in uvs) + .003, max(t.x for t in uvs) - .003
        v0, v1 = min(t.y for t in uvs) + .003, max(t.y for t in uvs) - .003
        for h in holes:
            poly = [Vector((u, 1.0 - v)) for u, v in h['outline']]     # Blender stores 1 - v
            if area2(poly) < 0:
                poly.reverse()
            centre = sum(poly, Vector((0, 0))) / len(poly)
            if not p.contains(centre):
                continue
            cut = clip(poly, u0, u1, v0, v1)
            if len(cut) < 3 or abs(area2(cut)) < MIN_SHARE * abs(area2(poly)):
                continue
            top = [p.point(t) for t in cut]
            n = p.normal
            mid = sum(top, Vector()) / len(top)
            hit = inner_tree.ray_cast(mid - n * .1, -n, 200.0) if inner_tree else (None,)
            gap = (hit[3] + .1) if hit[0] is not None else None
            if gap is not None:
                # the inside skin is not quite parallel: the shade and the sides stop above its nearest point,
                # the cutter goes past its farthest (near the eaves there may be no inside skin at all)
                hits = [inner_tree.ray_cast(q - n * .1, -n, 200.0) for q in top]
                depths = [h2[3] + .1 for h2 in hits if h2[0] is not None and h2[3] < gap * 1.5]
                gap = min(depths + [gap])
                reach = max(depths + [gap])
            else:
                reach = 10.0
            bottom = [q - n * (gap or 10.0) for q in top]
            deep = [q - n * reach for q in top]
            cutters.append((top, bottom, p, h, gap, cut, deep))
            g, gn = to_gothic(mid), to_gothic(n)
            print(f'hole {h["box"][0]:.2f}: centre {g.x:.0f} {g.y:.0f} {g.z:.0f} cm, normal {gn.x:.2f} {gn.y:.2f} {gn.z:.2f}, '
                  f'size {max(t.x for t in cut) * p.du.length - min(t.x for t in cut) * p.du.length:.0f} x '
                  f'{max(t.y for t in cut) * p.dv.length - min(t.y for t in cut) * p.dv.length:.0f} cm, gap {gap and round(gap)} cm')
    print(f'{len(panels)} roof triangles, {len(inner)} inside triangles, {len(cutters)} holes cut')

    # --- cut both skins with the prisms (exact boolean on a copy holding only the roof's faces)
    roof_faces = set(outer) | set(inner)
    loop_index = bm.loops.layers.int.new('loop_index')
    i = 0
    for f in bm.faces:
        for lp in f.loops:
            lp[loop_index] = i
            i += 1
    tmp = me.copy()
    tbm = bmesh.new()
    tbm.from_mesh(tmp)
    tbm.faces.ensure_lookup_table()
    keep = {f.index for f in roof_faces}
    bmesh.ops.delete(tbm, geom=[f for f in tbm.faces if f.index not in keep], context='FACES')
    tbm.to_mesh(tmp)
    tbm.free()
    roof_obj = bpy.data.objects.new('roof', tmp)
    bpy.context.scene.collection.objects.link(roof_obj)
    cm = bpy.data.meshes.new('cutter')
    cbm = bmesh.new()
    for top, bottom, p, h, gap, cut, deep in cutters:
        n = p.normal
        a = [cbm.verts.new(q + n * OVER) for q in top]
        b = [cbm.verts.new(q - n * OVER) for q in deep]
        cbm.faces.new(a)
        cbm.faces.new(list(reversed(b)))
        for k in range(len(a)):
            cbm.faces.new([a[k], b[k], b[(k + 1) % len(b)], a[(k + 1) % len(a)]])
    bmesh.ops.recalc_face_normals(cbm, faces=cbm.faces)
    cbm.to_mesh(cm)
    cbm.free()
    cutter = bpy.data.objects.new('cutter', cm)
    bpy.context.scene.collection.objects.link(cutter)
    roof_obj.matrix_world = obj.matrix_world
    cutter.matrix_world = obj.matrix_world
    mod = roof_obj.modifiers.new('holes', 'BOOLEAN')
    mod.operation, mod.solver, mod.object = 'DIFFERENCE', 'EXACT', cutter
    mod.use_hole_tolerant = True
    dg = bpy.context.evaluated_depsgraph_get()
    cut_me = bpy.data.meshes.new_from_object(roof_obj.evaluated_get(dg))

    # --- put the cut roof back: original vertices where positions match, new ones marked -1
    old_planes = [(f.normal.copy(), f.calc_center_median().copy()) for f in roof_faces]
    # the skins come back cut: their old outline may open where the cut moved it
    ctx.allow_open(roof_faces, 'outline of the roof skins before the cut')
    built = []
    bmesh.ops.delete(bm, geom=list(roof_faces), context='FACES_ONLY')
    bm.verts.ensure_lookup_table()
    existing = {pos_key(v.co): v for v in bm.verts if v.is_valid}
    vid = bm.verts.layers.float.get('_VERTEX')
    fid = bm.verts.layers.float.get('_FEATURE')
    color_point = bm.verts.layers.color.get(color_name) if color_name else None
    color_corner = bm.loops.layers.color.get(color_name) if color_name else None
    rbm = bmesh.new()
    rbm.from_mesh(cut_me)
    ruv = rbm.loops.layers.uv[uv_name]
    rcp = rbm.verts.layers.color.get(color_name) if color_name and color_point else None
    rcc = rbm.loops.layers.color.get(color_name) if color_name and color_corner else None
    added = 0

    class Lazy(dict):
        """Boolean vertex -> chunk vertex, made when a kept face first uses it: the cutter's own
        vertices never enter the chunk."""
        def __missing__(self, v):
            nonlocal added
            k = pos_key(v.co)
            if k not in existing:
                w = bm.verts.new(v.co)
                if vid is not None:
                    w[vid] = -1.0
                if fid is not None:
                    w[fid] = -1.0
                if color_point is not None and rcp is not None:
                    w[color_point] = v[rcp]
                existing[k] = w
                added += 1
            self[v] = existing[k]
            return self[v]
    vmap = Lazy()
    kept = 0
    for f in rbm.faces:
        c, nn = f.calc_center_median(), f.normal
        if not any(abs((c - o).dot(on)) < .5 and abs(nn.dot(on)) > .99 for on, o in old_planes):
            continue          # a face of the cutter, not of the skins
        pairs = []     # merged positions can repeat a vertex: drop the repeats
        for lp in f.loops:
            w = vmap[lp.vert]
            if not any(w is q for q, _ in pairs):
                pairs.append((w, lp))
        vs = [w for w, _ in pairs]
        if len(vs) < 3 or bm.faces.get(vs):
            continue
        g = bm.faces.new(vs)
        built.append(g)
        g.material_index = f.material_index
        for a, b in zip(g.loops, [lp for _, lp in pairs]):
            a[uv].uv = b[ruv].uv
            a[loop_index] = -1        # cut faces are flat, like the imported roof
            if color_corner is not None and rcc is not None:
                a[color_corner] = b[rcc]
        kept += 1
    rbm.free()
    print(f'roof back: {kept} faces, {added} new vertices')

    # --- sides of the holes, laths and rafters
    roof_colour = None
    if color_corner is not None:
        roof_colour = tuple(outer[0].loops[0][color_corner]) if outer and outer[0].is_valid else None

    def new_vert(co):
        w = bm.verts.new(co)
        if vid is not None:
            w[vid] = -1.0
        if fid is not None:
            w[fid] = -1.0
        return w

    def quad(pts, uvs, facing, slot=None):
        vs = [new_vert(q) for q in pts]
        f = bm.faces.new(vs)
        built.append(f)
        f.normal_update()
        if f.normal.dot(facing) < 0:
            f.normal_flip()
        f.material_index = wood_slot if slot is None else slot
        for lp in f.loops:
            k = vs.index(lp.vert)
            lp[uv].uv = uvs[k]
            lp[loop_index] = -1
        return f

    def beam(origin, along, across, down, length, width, depth):
        """A box: origin at the top centre of one end; `along` the grain, `across`, `down` (unit vectors)."""
        a0 = origin - across * (width / 2)
        corners = lambda s: [a0 + along * s, a0 + along * s + across * width,
                             a0 + along * s + across * width + down * depth, a0 + along * s + down * depth]
        e0, e1 = corners(0), corners(length)
        L = length / WOOD_UV
        sides = [(e0[0], e0[1], e1[1], e1[0], -down), (e0[1], e0[2], e1[2], e1[1], across),
                 (e0[2], e0[3], e1[3], e1[2], down), (e0[3], e0[0], e1[0], e1[3], -across)]
        for a, b, c, d, facing in sides:
            w = (b - a).length / WOOD_UV
            quad([a, b, c, d], [Vector((0, 0)), Vector((0, w)), Vector((L, w)), Vector((L, 0))], facing)
        for e, facing in ((e0, -along), (e1, along)):
            quad(e, [Vector((0, 0)), Vector((.02, 0)), Vector((.02, .02)), Vector((0, .02))], facing)

    for top, bottom, p, h, gap, cut, deep in cutters:
        n = p.normal
        centre = sum(top, Vector()) / len(top)
        run = 0.0
        def out(q):
            d = q - centre
            d -= n * d.dot(n)
            return q + d.normalized() * SEAM
        top_o = [out(q) + n * SEAM for q in top]
        bot_o = [out(q) for q in bottom]
        for k in range(len(top)):
            a, b = top_o[k], top_o[(k + 1) % len(top)]
            c, d = bot_o[(k + 1) % len(bottom)], bot_o[k]
            edge = (b - a).length
            facing = (centre - (a + b) / 2)
            facing -= n * facing.dot(n)
            quad([a, b, c, d], [Vector((run / WOOD_UV, 0)), Vector(((run + edge) / WOOD_UV, 0)),
                                Vector(((run + edge) / WOOD_UV, (gap or 10) / WOOD_UV)),
                                Vector((run / WOOD_UV, (gap or 10) / WOOD_UV))], facing, wall_slot)
            run += edge
        depth_room = (gap or 30.0) - 3.0
        # the shade over the hut's lit interior: a centimetre above the inside skin, seen only from outside
        if gap is not None:
            veil = [q + n * 1.0 for q in bottom]
            # the stepped outline is concave: triangulate it flat in the panel's UV, not as one n-gon
            for tri in tessellate_polygon([[(t.x, t.y, 0.0) for t in cut]]):
                quad([veil[k] for k in tri], [Vector((0, 0))] * 3, n, shade_slot)
        across_u = p.du.normalized()           # along the courses
        down_v = p.dv.normalized()             # V grows up the slope in Blender's UV
        us = [t.x for t in [Vector((u, 1 - v)) for u, v in h['outline']]]
        vs = [1 - v for u, v in h['outline']]
        if h.get('lath_v') is not None:
            lv = 1.0 - h['lath_v']
            t0 = Vector((min(us), lv))
            start = p.point(t0) - across_u * REACH
            length = (max(us) - min(us)) * p.du.length + 2 * REACH
            beam(start - n * SINK, across_u, down_v, -n, length, LATH[0], min(LATH[1], depth_room))
        if h.get('rafter_u') is not None:
            t0 = Vector((h['rafter_u'], min(vs)))
            start = p.point(t0) - down_v * REACH
            length = (max(vs) - min(vs)) * p.dv.length + 2 * REACH
            top_off = LATH[1] + SINK
            beam(start - n * top_off, down_v, across_u, -n, length, RAFTER[0], min(RAFTER[1], depth_room - LATH[1]))

    # the edges of the holes, the sides standing on their own, the free laths and rafters
    ctx.allow_open(built, 'cut edges of the holes, their sides, laths and rafters')
    bm.to_mesh(me)
    bm.free()
    me.validate()
    me.update()

    li = me.attributes['loop_index'].data
    poly_of_loop = [0] * len(me.loops)
    for poly in me.polygons:
        for k in poly.loop_indices:
            poly_of_loop[k] = poly.index
    normals = []
    for k in range(len(me.loops)):
        old = li[k].value
        normals.append(old_normals[old] if old >= 0 else Vector(me.polygons[poly_of_loop[k]].normal))
    me.normals_split_custom_set(normals)
    me.attributes.remove(me.attributes['loop_index'])
    for o in (roof_obj, cutter):
        bpy.data.objects.remove(o)

    tris = sum(len(poly.vertices) - 2 for poly in me.polygons)
    print(f'chunk: {len(me.polygons)} faces, {tris} triangles after the roof holes')
