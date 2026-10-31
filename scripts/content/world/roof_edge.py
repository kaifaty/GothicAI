"""The edge of the shingle roofs NW_CITY_ROOF_01 of the harbour huts: the butts and the thickness of the shingles.

An edit of world_edit.py on the chunks of the huts B031, B035, B036 and B037 (census of the harbour,
reports/content/2026-10-harbour-house/census.csv); in NEWWORLD/0_-1 it goes after roof_holes.py:

    python3 scripts/content/world/world_edit.py build harbour-roof-edge-newworld-0-1 --set roofedge-1 \
        --chunk NEWWORLD/0_-1 --from harbour-roof-holes-newworld-0-1 --edit scripts/content/world/roof_edge.py \
        --materials <working-8>/materials/materials.json

A roof panel is one plane: at the eaves and the verges the shingles end in a knife edge, the painted courses
have no thickness. The texture (our roofs-8 bake, shingle_bake.py --seed 1) lies on every hut panel with the
eaves at V 1.013-1.021, two centimetres above the butts of course 0, and the verges at U 0.02-0.04 and 0.96-0.97,
so the panel's edges run along the painted shingles. The edit adds the shingles' own ends there:
- at the eaves, every shingle of course 0 gets its butt where the texture paints it (data/content/world/
  roof01-shingles.json, roof_edge_layout.py): it runs on past the edge to its own uneven butt line, as thick as
  the bake made it (2.2-3.2 cm, hand-split shakes), with the keyways between shingles where the texture has
  them; under it a starter course 1.8 cm thick, set back 1.5 cm, its keyways halfway between;
- at the verges, the cut side of every course: thick at the butt, thinning out to the next course, a saw-tooth
  as every course rests on the one below; it ends at the barge rafter's inner edge and leaves the rafter clear.
Ridges stay: the texture paints the ridge board there. Edges where a panel's U and V both change (valleys,
hips, a panel cut by the chunk border) and edges with another roof panel beyond them are left as they are.
The shingles' tops take the roof's own texture: at the eaves the lower part of the shingles under V = 1, laid a
hair over the panel from inside the painted ridge-board sliver (the square's top band shows past V = 1 there). The ends - butts, cut sides,
keyways, undersides - take the roof's timber darkened (OWN_ROOF_HOLE_WALL, roof_hole_shade.py, the sides of the
holes): end grain is darker than a weathered face, and in the shingles' own texture the ends read as a light new
strip along every eave (first capture, 2026-10-30). Everything else in the chunk stays as it was.

The converted chunk is Gothic content (a derivative, user's decision 2026-10-24): it stays outside git.
"""
import json
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree

from world_bpy import gothic_name, to_gothic

ROOF = 'NW_CITY_ROOF_01'
END = 'OWN_ROOF_HOLE_WALL'     # shingle ends: the roof's timber darkened (roof_hole_shade.py), as in the holes' sides
WOOD = 'NW_CITY_WOODBEAM_02'   # END's record is this one's, renamed, where a chunk has no hole yet
WOOD_UV = 150.0                # cm of wood per texture repeat along the grain
# Huts of the harbour's poor quarter, Gothic cm (x0, x1, z0, z1): census centre +- half its size and 30 cm
BOXES = {'B031': (-185.0, 2960.0, -6310.0, -4290.0), 'B035': (689.0, 3539.0, -7.0, 4091.0),
         'B036': (2391.0, 4795.0, -100.0, 2472.0), 'B037': (2539.0, 4961.0, -4524.0, -1992.0)}
STARTER = 1.8          # cm: the starter course under the eaves' shingles
STARTER_BACK = 1.5     # cm its butt sits behind the shingle's
COVER_V = 0.03         # a ridge-board sliver this wide in V (or less) at an eave is covered
BOARD_V = 0.006        # the ridge board is painted down to V 0.994: an eave this far inside V = 1 shows it too
SINK = 0.4             # cm: a new top starts this far under the roof and comes out of it on a clean line (laid a
                       # hair over the roof it fought with it far off: a dotted line, reviewers 2026-10-30)
SHINGLE_V = (0.07, 0.03)    # the covering top takes the shingles this far under V = 1, inner and outer edge: clear of
                            # the ridge board, painted down to V 0.994 (a light line along the eaves: reviewers, 2026-10-30)
EAVE_OVER = 3.0        # cm the butts stand past the fascia, which is flush with the panel's edge: a drip edge
EXT = (3.0, 6.5)       # cm: a butt stands this far past the panel's edge at least, at most
VERGE_OVER = (0.0, 0.0)   # cm the course ends overhang the barge rafter: none, they end at its inner edge (over it
                          # they halved the rafter's width in sight: reviewers, 2026-10-30)
VERGE_LIP = 4.0        # cm inside the edge where the end shingle starts to rise off the painted field
VERGE_HEAD = 0.0       # cm a course's end stands above the field just under the next butt: it thins out to nothing,
                       # so its side is a shadow at each butt with gaps between (at 0.8 cm the sides ran on into one
                       # black line along the barge rafter, reviewers 2026-10-30)
LIFT = 0.3             # cm the eaves' shingles lie over the roof's plane past its edge
ISO = 0.01             # an edge is an eave or a verge when V (U) changes by less than this along it
EDGE_UV = (0.001, 0.005)   # the verge lip takes the texture this far inside the edge (UV), outer and inner
NEIGHBOUR = 12.0       # cm: another roof panel this close past an edge makes it a joint, not an edge
SHINGLES = Path(__file__).resolve().parents[3] / 'data/content/world/roof01-shingles.json'
INPUTS = [SHINGLES]
PARAMETERS = {'boxes_cm': {k: list(v) for k, v in BOXES.items()}, 'starter_cm': STARTER,
              'starter_back_cm': STARTER_BACK, 'eave_over_cm': EAVE_OVER, 'butt_past_edge_cm': list(EXT),
              'verge_over_cm': list(VERGE_OVER), 'verge_lip_cm': VERGE_LIP, 'verge_head_cm': VERGE_HEAD}


def in_boxes(co):
    g = to_gothic(co)
    return any(x0 <= g.x <= x1 and z0 <= g.z <= z1 for x0, x1, z0, z1 in BOXES.values())


class Layout:
    """The shingles of roofs-8 in texture metres: x along U, y up the slope from V = 1."""

    def __init__(self, data):
        self.px, self.py = data['period_m']
        self.lines = data['course_lines_m']
        self.courses = {}
        for s in data['shingles']:
            self.courses.setdefault(s['course'], []).append(s)

    def course_at(self, y):
        """(course, its butt's y) of the course whose shingles show at y, the square's copies counted."""
        k = (y - self.lines[0]) // self.py
        base = y - k * self.py
        c = max(i for i, line in enumerate(self.lines) if line <= base + 1e-9)
        return c, self.lines[c] + k * self.py

    def shingles(self, course, x0, x1, dy):
        """Shingles of a course over x0..x1 (unwrapped), shifted by dy up the slope: (xl, xr, butt(x), cm)."""
        out = []
        k0, k1 = int((min(x0, x1) // self.px)) - 1, int((max(x0, x1) // self.px)) + 1
        for k in range(k0, k1 + 1):
            for s in self.courses[course]:
                xl, xr = s['x'][0] + k * self.px, s['x'][1] + k * self.px
                if xr < min(x0, x1) or xl > max(x0, x1):
                    continue
                ys = s['butt_y']
                xs = [xl + (xr - xl) * i / (len(ys) - 1) for i in range(len(ys))]
                # a broken corner or a hole's edge is not the butt line: keep the butt within 5 cm of its median
                med = sorted(ys)[len(ys) // 2]
                ys = [y if abs(y - med) < .05 else med for y in ys]
                out.append((xl, xr, xs, [y + dy for y in ys], s['butt_cm']))
        return sorted(out)


def interp(x, xs, ys):
    if x <= xs[0]:
        return ys[0]
    for a, b, ya, yb in zip(xs, xs[1:], ys, ys[1:]):
        if x <= b:
            return ya + (yb - ya) * (x - a) / (b - a)
    return ys[-1]


def outline_edges(underlay):
    """Open edges of the huts' roof panels in the original: (a, b, uv a, uv b, normal, inward point, colour)."""
    me = underlay.data
    names = [gothic_name(m) for m in me.materials]
    slot = names.index(ROOF)
    uv = me.uv_layers.active.data
    col = me.color_attributes.active_color
    roof = [p for p in me.polygons if p.material_index == slot and in_boxes(Vector(p.center))]
    key = lambda co: tuple(round(c, 1) for c in co)
    edges = {}
    for p in roof:
        vs = list(p.vertices)
        for i, lp in enumerate(p.loop_indices):
            j = p.loop_indices[(i + 1) % len(vs)]
            a, b = me.vertices[vs[i]].co, me.vertices[vs[(i + 1) % len(vs)]].co
            k = tuple(sorted((key(a), key(b))))
            edges.setdefault(k, []).append((p, lp, j))
    # every roof panel of the chunk, for the neighbour test
    allroof = [p for p in me.polygons if p.material_index == slot]
    tree = BVHTree.FromPolygons([v.co for v in me.vertices], [list(p.vertices) for p in allroof])
    out = []
    for k, users in edges.items():
        if len(users) != 1:
            continue
        p, la, lb = users[0]
        a, b = me.vertices[me.loops[la].vertex_index].co.copy(), me.vertices[me.loops[lb].vertex_index].co.copy()
        ua, ub = Vector(uv[la].uv), Vector(uv[lb].uv)
        third = Vector(p.center)
        n = Vector(p.normal)
        if n.z < 0:
            n = -n
        colour = tuple(col.data[la].color) if col is not None and col.domain == 'CORNER' else None
        out.append((a, b, ua, ub, n, third, colour, tree, gradient(me, p)))
    return out


def gradient(me, p):
    """(dP/du, dP/dv) of a polygon, world cm per UV unit, v as glTF stores it (from the top)."""
    uv = me.uv_layers.active.data
    l0, l1, l2 = list(p.loop_indices)[:3]
    p0, p1, p2 = (me.vertices[me.loops[k].vertex_index].co for k in (l0, l1, l2))
    t0, t1, t2 = (Vector((uv[k].uv.x, 1.0 - uv[k].uv.y)) for k in (l0, l1, l2))
    e1, e2 = p1 - p0, p2 - p0
    s1, s2 = t1 - t0, t2 - t0
    det = s1.x * s2.y - s1.y * s2.x
    return (e1 * s2.y - e2 * s1.y) / det, (e2 * s1.x - e1 * s2.x) / det


def edit(ctx):
    layout = Layout(json.loads(SHINGLES.read_text()))
    me = ctx.mesh
    names = [gothic_name(m) for m in me.materials]
    slot = names.index(ROOF)
    if END in names:
        end_slot = names.index(END)
    else:
        m = bpy.data.materials.new(END)
        record = me.materials[names.index(WOOD)]['gothic'].to_dict()
        record.pop('index', None)
        record.update(name=END, texture=END + '.TGA')
        m['gothic'] = record
        me.materials.append(m)
        end_slot = len(me.materials) - 1
    uv_name = me.uv_layers.active.name
    color_name = me.color_attributes.active_color.name if me.color_attributes.active_color else None
    old_normals = [Vector(n.vector) for n in me.corner_normals]

    bm = bmesh.new()
    bm.from_mesh(me)
    uv = bm.loops.layers.uv[uv_name]
    vid = bm.verts.layers.float.get('_VERTEX')
    fid = bm.verts.layers.float.get('_FEATURE')
    color_point = bm.verts.layers.color.get(color_name) if color_name else None
    color_corner = bm.loops.layers.color.get(color_name) if color_name else None
    loop_index = bm.loops.layers.int.new('loop_index')
    i = 0
    for f in bm.faces:
        for lp in f.loops:
            lp[loop_index] = i
            i += 1

    built = []
    colour = [None]

    def new_vert(co):
        w = bm.verts.new(co)
        if vid is not None:
            w[vid] = -1.0
        if fid is not None:
            w[fid] = -1.0
        if color_point is not None and colour[0] is not None:
            w[color_point] = colour[0]
        return w

    grain = [Vector((1.0, 0.0, 0.0)), Vector((0.0, 0.0, 1.0))]

    def end(pts, facing):
        """A shingle's end grain, butt or cut side: the darkened timber, its grain along the edge."""
        uvs = [Vector((q.dot(grain[0]) / WOOD_UV, q.dot(grain[1]) / WOOD_UV)) for q in pts]
        return face(pts, uvs, facing, end_slot)

    def face(pts, uvs, facing, material=None):
        keep = [k for k in range(len(pts)) if (pts[k] - pts[k - 1]).length >= 1e-3]     # a corner on its neighbour
        pts, uvs = [pts[k] for k in keep], [uvs[k] for k in keep]
        if len(pts) < 3:
            return None
        vs = [new_vert(q) for q in pts]
        f = bm.faces.new(vs)
        built.append(f)
        f.normal_update()
        if f.normal.dot(facing) < 0:
            f.normal_flip()
        f.material_index = slot if material is None else material
        for lp in f.loops:
            t = uvs[vs.index(lp.vert)]
            lp[uv].uv = (t.x, 1.0 - t.y)              # uvs in glTF v; Blender stores 1 - v
            lp[loop_index] = -1
            if color_corner is not None and colour[0] is not None:
                lp[color_corner] = colour[0]
        return f

    counts = {'eave': 0, 'verge': 0, 'ridge': 0, 'skipped': 0, 'shingles': 0}
    for a, b, ua, ub, n, inside, edge_colour, tree, (grad_u, grad_v) in outline_edges(ctx.underlay):
        colour[0] = edge_colour
        ua, ub = Vector((ua.x, 1.0 - ua.y)), Vector((ub.x, 1.0 - ub.y))     # glTF v from here on
        d = b - a
        length = d.length
        along = d / length
        out = along.cross(n)
        if out.dot(inside - (a + b) / 2) > 0:
            out = -out
        grain[0], grain[1] = along, n
        down = Vector((0.0, 0.0, -1.0))
        slope_down = down - n * down.dot(n)
        slope_down.normalize()
        facing = out.dot(slope_down)
        du, dv = ub.x - ua.x, ub.y - ua.y
        if facing < -.7:
            counts['ridge'] += 1
            continue
        mid = (a + b) / 2
        hit = tree.find_nearest(mid + out * (NEIGHBOUR + 10.0))
        if hit[0] is not None and hit[3] < NEIGHBOUR:
            counts['skipped'] += 1        # another panel goes on past this edge
            continue
        # world cm per texture metre along U and V, from the panel: the edge's own direction gives one of them
        if facing > .7 and abs(dv) < ISO and abs(du) > 1e-6:
            counts['eave'] += 1
            # world cm per texture metre up the slope: the butts' offsets are texture metres
            cm_per_m_v = grad_v.length / layout.py
            v_edge = (ua.y + ub.y) / 2
            y_edge = (1.0 - v_edge) * layout.py
            # the square's top band (the painted ridge board) runs past V = 1 onto the eaves: cover it
            v_floor = float(round(v_edge))
            cover = (v_edge - v_floor + BOARD_V) * grad_v.length if -BOARD_V < v_edge - v_floor < COVER_V else 0.0
            course, butt_line = layout.course_at(y_edge)
            x_a, x_b = ua.x * layout.px, ub.x * layout.px
            sign = 1 if x_b > x_a else -1
            t_of = lambda x: (x - x_a) / (x_b - x_a)

            def point(t, ext, depth):
                return a + d * t + out * ext - n * depth

            row = layout.shingles(course, x_a, x_b, butt_line - layout.lines[course])
            starters = []
            for xl, xr, xs, ys, thick in row:
                counts['shingles'] += 1
                t0, t1 = sorted((max(0.0, min(1.0, t_of(xl))), max(0.0, min(1.0, t_of(xr)))))
                if t1 - t0 < 1e-3:
                    continue
                ts = [t0, (t0 + t1) / 2, t1]
                exts = []
                for t in ts:
                    x = x_a + (x_b - x_a) * t
                    e = EAVE_OVER + (y_edge - interp(x, xs, ys)) * cm_per_m_v
                    exts.append(max(EXT[0], min(EXT[1], e)))
                # the shingle's top: from under the roof inside the painted ridge-board sliver (the square's top band
                # shows past V = 1 at the eaves) up out of it before the sliver, over the edge, out to its butt, a
                # hair above the roof's plane, in the shingle's own lower part
                inner = -(3.0 * cover + 3.0)
                top_in = [point(t, inner, SINK) for t in ts]
                top_edge = [point(t, 0.0, -LIFT) for t in ts]
                top = [point(t, e, -LIFT) for t, e in zip(ts, exts)]
                low = [point(t, e, thick) for t, e in zip(ts, exts)]
                low_in = [point(t, 0.0, thick) for t in ts]
                uv_in = [Vector((ua.x + du * t, v_floor - SHINGLE_V[0])) for t in ts]
                uv_out = [Vector((ua.x + du * t, v_floor - SHINGLE_V[1])) for t in ts]
                uv_edge = [i.lerp(o, -inner / (e - inner)) for i, o, e in zip(uv_in, uv_out, exts)]
                for k in range(2):
                    # the top, the butt, the underside back to the edge
                    face([top_in[k], top_in[k + 1], top_edge[k + 1], top_edge[k]],
                         [uv_in[k], uv_in[k + 1], uv_edge[k + 1], uv_edge[k]], n)
                    face([top_edge[k], top_edge[k + 1], top[k + 1], top[k]],
                         [uv_edge[k], uv_edge[k + 1], uv_out[k + 1], uv_out[k]], n)
                    end([top[k], top[k + 1], low[k + 1], low[k]], out)
                    end([low[k], low[k + 1], low_in[k + 1], low_in[k]], -n)
                for j, side in ((0, -along), (2, along)):
                    if 0.0 < ts[j] < 1.0:      # a keyway: the side of the shingle
                        end([top_edge[j], top[j], low[j], low_in[j]], side)
                    else:                      # the eave's end at a verge: the shingle's cut side, in its own tone
                        dv_cm = Vector((0.0, thick / grad_v.length))
                        face([top_edge[j], top[j], low[j], low_in[j]],
                             [uv_edge[j], uv_out[j], uv_out[j] + dv_cm, uv_edge[j] + dv_cm], side)
                starters.append((ts[1], exts[1], thick))
            # the starter course: keyways halfway along the shingles above, its butt 1.5 cm behind theirs
            cuts = [0.0] + [t for t, _, _ in starters] + [1.0]
            cuts = sorted(set(max(0.0, min(1.0, t)) for t in cuts))
            for t0, t1 in zip(cuts, cuts[1:]):
                if t1 - t0 < 1e-3:
                    continue
                tm = (t0 + t1) / 2
                near = min(starters, key=lambda s: abs(s[0] - tm)) if starters else (tm, EXT[0], 2.5)
                ext = max(0.0, near[1] - STARTER_BACK)
                top_d = near[2]
                gap = 0.4 / length               # 4 mm keyway, in edge units
                s0, s1 = (t0 + gap if t0 > 0 else t0), (t1 - gap if t1 < 1 else t1)
                q = [point(s0, ext, top_d), point(s1, ext, top_d), point(s1, ext, top_d + STARTER),
                     point(s0, ext, top_d + STARTER)]
                qi = [point(s0, 0.0, top_d + STARTER), point(s1, 0.0, top_d + STARTER)]
                end(q, out)
                end([point(s0, 0.0, top_d), point(s1, 0.0, top_d), q[1], q[0]], n)   # seen down a keyway
                end([q[3], q[2], qi[1], qi[0]], -n)
                for s, e, side in ((s0, (q[0], q[3], qi[0]), -along), (s1, (q[1], q[2], qi[1]), along)):
                    if 0.0 < s < 1.0 and ext > 0.0:
                        end([point(s, 0.0, top_d), e[0], e[1], e[2]], side)
        elif abs(facing) <= .7 and abs(du) < ISO and abs(dv) > 1e-6:
            counts['verge'] += 1
            u_edge = (ua.x + ub.x) / 2
            y_a, y_b = (1.0 - ua.y) * layout.py, (1.0 - ub.y) * layout.py
            lo, hi = sorted((y_a, y_b))
            u_in = EDGE_UV[1] if out.dot(grad_u) < 0 else -EDGE_UV[1]     # toward the panel
            x_edge = u_edge * layout.px
            t_of = lambda y: (y - y_a) / (y_b - y_a)
            # the course lines over the edge, the square's copies counted
            ys = []
            c, butt = layout.course_at(lo)
            y = butt
            while y < hi:
                ys.append((y, c))
                c = (c + 1) % len(layout.lines)
                k = (y - layout.lines[0]) // layout.py
                nxt = layout.lines[c] + (k + (1 if c == 0 else 0)) * layout.py
                y = nxt if nxt > y else nxt + layout.py
            ys.append((y, c))
            up = d if y_b > y_a else -d                 # up the slope along the edge
            for (y0, c0), (y1, _) in zip(ys, ys[1:]):
                s0, s1 = max(lo, y0), min(hi, y1)
                if s1 - s0 < 1e-3:
                    continue
                row = layout.shingles(c0, x_edge - .01, x_edge + .01, y0 - layout.lines[c0])
                thick = row[0][4] if row else 2.6
                # the end shingle rises off the field and overhangs the barge rafter; thicker shingles further
                over = VERGE_OVER[0] + (VERGE_OVER[1] - VERGE_OVER[0]) * max(0.0, min(1.0, (thick - 2.2) / 1.0))
                th = lambda yy: thick - (thick - VERGE_HEAD) * (yy - y0) / (y1 - y0)

                def p(yy, o, h):
                    return a + d * t_of(yy) + out * o + n * h

                v0, v1 = 1.0 - s0 / layout.py, 1.0 - s1 / layout.py
                ui, uo = Vector((u_edge + u_in * 3, 0)), Vector((u_edge + u_in * .2, 0))
                uvs = lambda u, va, vb: [Vector((u.x, va)), Vector((u.x, vb))]
                face([p(s0, -VERGE_LIP, -SINK), p(s1, -VERGE_LIP, -SINK), p(s1, over, th(s1)), p(s0, over, th(s0))],
                     uvs(ui, v0, v1) + uvs(uo, v1, v0), n)
                # the course's cut side over the barge rafter: long grain, the shingle's own tone, the texture
                # folded over the edge; only the butts are end grain, a dark notch at every course
                wrap = lambda h: Vector((uo.x + u_in / EDGE_UV[1] * h / grad_u.length, 0))
                face([p(s0, over, th(s0)), p(s1, over, th(s1)), p(s1, over, 0.0), p(s0, over, 0.0)],
                     uvs(uo, v0, v1) + uvs(wrap(th(s1)), v1, v0), out)
                if s0 == y0:
                    # the butt of the course, seen from down the slope
                    end([p(y0, -VERGE_LIP, -SINK), p(y0, over, th(y0)), p(y0, over, 0.0)], -up)
        else:
            counts['skipped'] += 1

    print(f'roof edge: {counts}')
    # the ends stand on their own along the panel's edge; their back and inner sides are under the roof
    ctx.allow_open(built, 'shingle butts at the eaves and course ends at the verges, against the roof edge')
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
    tris = sum(len(poly.vertices) - 2 for poly in me.polygons)
    print(f'chunk: {len(me.polygons)} faces, {tris} triangles after the roof edge')

