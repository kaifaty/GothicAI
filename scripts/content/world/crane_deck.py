"""The plank deck of the harbour cranes NW_HARBOUR_WOOD_IRON_01: the planks' thickness and ends, the straps in relief.

An edit of world_edit.py on the chunks of the four harbour cranes (NEWWORLD/-2_-2, -2_-3, -2_-4, -1_-1, -1_0):

    python3 scripts/content/world/world_edit.py build harbour-crane-deck-newworld-1-1 --set cranedeck-1 \
        --chunk NEWWORLD/-1_-1 --edit scripts/content/world/crane_deck.py

The harbour has no wooden piers: its quay and the pier to the paladins' ship are stone. NW_HARBOUR_WOOD_IRON_01
lies on the four wooden cranes, and the biggest piece of each is the deck the crane stands on, a slab 3.3 x 8 m
and 46-50 cm thick with its corners cut. The texture paints the deck as three broad planks
along U with four nailed straps across them (U 0.078, 0.342, 0.659, 0.918, each 0.05 wide); the slab's top is one
plane and its sides go straight down from its edge, so the planks have no thickness and the straps no relief. The
edit adds:
- round the deck's edge, the planks' own edge: they stand 3 cm past the slab's side and are 8 cm thick, with a
  shadow under them;
- at the ends, the boards' own ends: every painted plank (1.1 m) is four boards there, 2 cm apart, the planks 3 cm
  apart at the texture's seams; each board stands out its own way (0-4 cm more) and is its own thickness (7.2-8.7
  cm), and its end shows end grain, the weathered rings of NW_NATURE_BARKRINGS_02, each board a different part of
  them. The boards at a corner keep the edge's line, so the edges meet. No joint is cut within 20 cm of a corner;
- the straps, 2 cm proud of the planks, across the deck where the texture paints them, ending at the slab's edge.
The planks' top and sides take the deck's own texture; the top of the edge takes the board 8 cm in
from the edge, where the moss is (the edge's own texels are the painted dark border). The first versions squeezed
the board into the ends and read as a separate rim board with the grain along it, and bent the straps over the
edge into dark hooks (reviewers, 2026-10-30). Everything else in the chunk stays as it was.

A deck can lie across a chunk border: every chunk's edit reads the decks from the base chunks around it, builds
the edge where its own top triangle is, and the straps in the chunk holding most of the deck's top.

The converted chunk is Gothic content (a derivative, user's decision 2026-10-24): it stays outside git.
"""
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector

from world_bpy import gothic_name

sys.path.insert(0, str(Path(__file__).resolve().parent))
import chunk_glb  # noqa: E402

MATERIAL = 'NW_HARBOUR_WOOD_IRON_01'
ENDS = 'NW_NATURE_BARKRINGS_02'   # the boards' end grain: weathered rings of the original, as on the harbour's stumps
ENDS_FROM = '-1_-1'    # a base chunk holding ENDS' record, for the chunks that have none
RING_CM = 60.0         # cm of end grain per repeat of ENDS
MIN_DECK = 15.0e4      # cm²: a horizontal top this large is a crane's deck (the feet and posts are under 4 m²)
OVER = 3.0             # cm the planks stand past the slab's side
PLANK = 8.0            # cm: the planks' thickness
SEAM = 3.0             # cm between plank ends at a seam
BOARDS = (0.27, 0.52, 0.77)    # board joints within a painted plank, at the ends
BOARD_GAP = 2.0        # cm between board ends: 1-1.5 cm read as nothing in the crane's shadow
SEAM_MIN = 20.0        # cm: a joint closer than this to a corner is not cut (no sliver of a board)
BOARD_OVER = (0.0, 3.0, 1.0, 4.0, 1.5, 0.5, 2.5)     # cm a board's end stands out further, board by board
BOARD_THICK = (0.0, 0.7, -0.5, 0.3, -0.8, 0.5)       # cm a board is thicker than PLANK
# where a board's end lies on ENDS (its window's corner), light weathered heartwood and dark outer rings by turns:
# mean grey of the window 69, 60, 66, 54, 66, 49, 68 (of 255)
RING_AT = ((0.30, 0.42), (0.35, 0.30), (0.15, 0.50), (0.45, 0.70), (0.00, 0.60), (0.40, 0.00), (0.25, 0.45))
TOP_IN = 8.0           # cm in from the edge the top of the edge takes the board from: the moss, past the painted border
SEAMS_V = (1 / 3, 2 / 3)                       # plank seams of the texture, V of the square
PLANKS = len(SEAMS_V) + 1
STRAPS_U = (0.078, 0.342, 0.659, 0.918)        # strap centres of the texture, U of the square
STRAP_W = 0.05         # strap width, U of the square
STRAP = 2.0            # cm the straps stand proud of the planks
PARAMETERS = {'material': MATERIAL, 'ends': ENDS, 'ring_cm': RING_CM, 'over_cm': OVER, 'plank_cm': PLANK,
              'seam_cm': SEAM, 'boards': list(BOARDS), 'board_gap_cm': BOARD_GAP, 'seam_min_cm': SEAM_MIN,
              'board_over_cm': list(BOARD_OVER), 'board_thick_cm': list(BOARD_THICK), 'top_in_cm': TOP_IN,
              'seams_v': list(SEAMS_V), 'straps_u': list(STRAPS_U), 'strap_w_u': STRAP_W, 'strap_cm': STRAP}


def blender(p):
    """glTF (x, y, z) cm -> the Blender mesh's (x, -z, y)."""
    return Vector((float(p[0]), -float(p[2]), float(p[1])))


def deck_triangles(chunk_path):
    """Up-facing triangles of the material in a base chunk: [(3 Blender points, 3 glTF uvs)]."""
    if not chunk_path.is_file():
        return []
    out = []
    for prim in chunk_glb.read_chunk(chunk_path).primitives:
        if prim.material.get('name') != MATERIAL:
            continue
        for pos, uv in zip(prim.positions, prim.uvs):
            pts = [blender(p) for p in pos]
            n = (pts[1] - pts[0]).cross(pts[2] - pts[0])
            if n.length > 1e-6 and n.normalized().z > .99:          # the slab's top; its underside faces down
                out.append((pts, [Vector((float(t[0]), float(t[1]))) for t in uv]))
    return out


def decks(ctx):
    """Decks around this chunk: lists of (points, uvs, chunk) triangles of one top plane, over MIN_DECK."""
    x, z = (int(v) for v in ctx.chunk.split('_'))
    tris = []
    for dx in (-1, 0, 1):
        for dz in (-1, 0, 1):
            name = f'{x + dx}_{z + dz}'
            tris += [(p, t, name) for p, t in deck_triangles(ctx.base_chunks / f'{name}.glb')]
    key = lambda co: (round(co.x, 1), round(co.y, 1), round(co.z, 1))
    parent = list(range(len(tris)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    seen = {}
    for i, (pts, _, _) in enumerate(tris):
        for p in pts:
            k = key(p)
            if k in seen:
                parent[find(i)] = find(seen[k])
            else:
                seen[k] = i
    groups = {}
    for i in range(len(tris)):
        groups.setdefault(find(i), []).append(tris[i])
    area = lambda t: (t[0][1] - t[0][0]).cross(t[0][2] - t[0][0]).length / 2
    # a top: one height
    return [g for g in groups.values() if sum(area(t) for t in g) >= MIN_DECK and
            max(p.z for t in g for p in t[0]) - min(p.z for t in g for p in t[0]) < 1.0]


class Mapping:
    """The deck's texture mapping, from its largest triangle: P(u, v) = p0 + du (u - u0) + dv (v - v0), glTF v."""

    def __init__(self, tris):
        pts, uvs, _ = max(tris, key=lambda t: (t[0][1] - t[0][0]).cross(t[0][2] - t[0][0]).length)
        self.p0, self.t0 = pts[0], uvs[0]
        e1, e2 = pts[1] - pts[0], pts[2] - pts[0]
        s1, s2 = uvs[1] - uvs[0], uvs[2] - uvs[0]
        det = s1.x * s2.y - s1.y * s2.x
        self.du = (e1 * s2.y - e2 * s1.y) / det
        self.dv = (e2 * s1.x - e1 * s2.x) / det
        n = self.du.cross(self.dv)
        m = [[self.du.x, self.dv.x], [self.du.y, self.dv.y]]
        self.det = m[0][0] * m[1][1] - m[0][1] * m[1][0]
        self.inv = [[m[1][1] / self.det, -m[0][1] / self.det], [-m[1][0] / self.det, m[0][0] / self.det]]
        self.error = max((self.uv(p) - t).length for tri in tris for p, t in zip(tri[0], tri[1]))

    def point(self, t):
        return self.p0 + self.du * (t.x - self.t0.x) + self.dv * (t.y - self.t0.y)

    def uv(self, p):
        d = p - self.p0
        return Vector((self.t0.x + self.inv[0][0] * d.x + self.inv[0][1] * d.y,
                       self.t0.y + self.inv[1][0] * d.x + self.inv[1][1] * d.y))


def outline(tris):
    """Edges used by one triangle of the deck: (a, b, third point, chunk)."""
    key = lambda co: (round(co.x, 1), round(co.y, 1), round(co.z, 1))
    edges = {}
    for pts, _, chunk in tris:
        for k in range(3):
            a, b, c = pts[k], pts[(k + 1) % 3], pts[(k + 2) % 3]
            kk = tuple(sorted((key(a), key(b))))
            edges.setdefault(kk, []).append((a, b, c, chunk))
    return [e[0] for e in edges.values() if len(e) == 1]


def clip(poly, u0, u1):
    """Sutherland-Hodgman: the part of a UV polygon with u0 <= u <= u1."""
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
    at = lambda u: (lambda a, b: a.lerp(b, (u - a.x) / (b.x - a.x)))
    poly = side(poly, lambda p: p.x >= u0, at(u0))
    return side(poly, lambda p: p.x <= u1, at(u1)) if poly else []


def loop(edges):
    """The outline as one closed polygon of points (the deck is one convex slab)."""
    key = lambda co: (round(co.x, 1), round(co.y, 1))
    nxt = {}
    for a, b, _, _ in edges:
        nxt.setdefault(key(a), []).append(b)
        nxt.setdefault(key(b), []).append(a)
    start = edges[0][0]
    poly, prev, cur = [start], None, start
    for _ in range(len(edges)):
        cands = [p for p in nxt[key(cur)] if prev is None or key(p) != key(prev)]
        if not cands:
            break
        prev, cur = cur, cands[0]
        if key(cur) == key(start):
            break
        poly.append(cur)
    return poly


def edit(ctx):
    me = ctx.mesh
    names = [gothic_name(m) for m in me.materials]
    slot = names.index(MATERIAL)
    if ENDS in names:
        ends_slot = names.index(ENDS)
    else:
        doc, _ = chunk_glb.read_glb(ctx.base_chunks / f'{ENDS_FROM}.glb')
        record = next(m['extras']['gothic'] for m in doc['materials'] if m.get('name') == ENDS)
        mat = bpy.data.materials.new(ENDS)
        mat['gothic'] = record
        me.materials.append(mat)
        ends_slot = len(me.materials) - 1
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
    # the deck's vertex colour: the engine's light baked into the world mesh; new faces take the deck's
    colour = None
    deck_faces = [f for f in bm.faces if f.material_index == slot and f.normal.z > .99]
    if deck_faces and color_corner is not None:
        colour = tuple(max(deck_faces, key=lambda f: f.calc_area()).loops[0][color_corner])

    built = []

    def face(pts, uvs, facing, material=None):
        if any((pts[k] - pts[k - 1]).length < 1e-3 for k in range(len(pts))):
            return None
        vs = []
        for q in pts:
            w = bm.verts.new(q)
            if vid is not None:
                w[vid] = -1.0
            if fid is not None:
                w[fid] = -1.0
            if color_point is not None and colour is not None:
                w[color_point] = colour
            vs.append(w)
        f = bm.faces.new(vs)
        built.append(f)
        f.normal_update()
        if f.normal.dot(facing) < 0:
            f.normal_flip()
        f.material_index = slot if material is None else material
        for lp in f.loops:
            t = uvs[vs.index(lp.vert)]
            lp[uv].uv = (t.x, 1.0 - t.y)
            lp[loop_index] = -1
            if color_corner is not None and colour is not None:
                lp[color_corner] = colour
        return f

    up = Vector((0.0, 0.0, 1.0))
    found = decks(ctx)
    print(f'crane decks around {ctx.chunk}: {len(found)}')
    for tris in found:
        m = Mapping(tris)
        area = {}
        for pts, _, chunk in tris:
            area[chunk] = area.get(chunk, 0.0) + (pts[1] - pts[0]).cross(pts[2] - pts[0]).length / 2
        owner = max(area, key=area.get)
        edges = outline(tris)
        centre = sum((p for t in tris for p in t[0]), Vector()) / (3 * len(tris))
        g = Vector((-centre.x, centre.z, -centre.y))
        print(f'deck at {g.x:.0f} {g.y:.0f} {g.z:.0f} cm: {sum(area.values()) / 1e4:.1f} m², {len(edges)} edges, '
              f'mapping error {m.error:.4f}, straps in {owner}')
        straps = [k + c for k in range(-2, 4) for c in STRAPS_U]

        # --- the planks' edge round the deck
        corners = {}
        for a, b, third, chunk in edges:
            if chunk != ctx.chunk:
                continue
            d = b - a
            length = d.length
            out = d.cross(up).normalized()
            if out.dot(third - a) > 0:
                out = -out
            for p in (a, b):
                corners.setdefault((round(p.x, 1), round(p.y, 1)), (p, []))[1].append((out, m.uv(p)))
            ta, tb = m.uv(a), m.uv(b)
            per_cm = (m.uv(a - out * 10.0) - ta) / 10.0                    # UV per cm into the deck
            v_cm = Vector((0.0, (m.uv(a + m.dv.normalized()) - ta).length))  # UV per cm across the grain
            # An end (V changes along the edge) shows the boards' ends; a long side (U changes) and a cut corner
            # the plank's side at its true scale, the grain running along.
            across = abs(tb.y - ta.y) > 2.0 * abs(tb.x - ta.x)
            cuts, joints = {0.0, 1.0}, {}
            if across:
                for k in range(-3, 6):
                    for j, f in enumerate((0.0,) + BOARDS):
                        v = (k + f) / PLANKS
                        if (ta.y - v) * (tb.y - v) < 0:
                            t = (v - ta.y) / (tb.y - ta.y)
                            if min(t, 1.0 - t) * length >= SEAM_MIN:    # no sliver of a board at a corner
                                cuts.add(t)
                                joints[t] = (SEAM if j == 0 else BOARD_GAP) / 2 / length
            cuts = sorted(cuts)
            print(f'  edge {length:.0f} cm, uv {ta.x:.3f} {ta.y:.3f} .. {tb.x:.3f} {tb.y:.3f}: '
                  f'{"end, " + str(len(cuts) - 1) + " boards" if across else "side"}')
            for t0, t1 in zip(cuts, cuts[1:]):
                s0, s1 = t0 + joints.get(t0, 0.0), t1 - joints.get(t1, 0.0)
                if s1 - s0 < 1e-4:
                    continue
                u0, u1 = ta.lerp(tb, s0), ta.lerp(tb, s1)
                e0, e1 = a + d * s0, a + d * s1
                over, thick = OVER, PLANK
                v = (u0.y + u1.y) / 2 * PLANKS
                g = int(v // 1) * (len(BOARDS) + 1) + sum(v % 1 > f for f in BOARDS)     # the board's number
                if across and t0 > 0.0 and t1 < 1.0:      # a board of its own; the corner ones keep the edge's line
                    over += BOARD_OVER[g % len(BOARD_OVER)]
                    thick += BOARD_THICK[g % len(BOARD_THICK)]
                o0, o1 = e0 + out * over, e1 + out * over
                l0, l1 = o0 - up * thick, o1 - up * thick
                i0, i1 = e0 - up * thick, e1 - up * thick
                side_v = v_cm * thick
                top0, top1 = u0 + per_cm * TOP_IN, u1 + per_cm * TOP_IN
                face([e0, e1, o1, o0], [top0, top1, top1 - per_cm * over, top0 - per_cm * over], up)
                if across:
                    rx, ry = RING_AT[g % len(RING_AT)]
                    w = (e1 - e0).length / RING_CM
                    r = [Vector((rx, ry)), Vector((rx + w, ry)), Vector((rx + w, ry + thick / RING_CM)),
                         Vector((rx, ry + thick / RING_CM))]
                    face([o0, o1, l1, l0], r, out, ends_slot)
                else:
                    face([o0, o1, l1, l0], [u0, u1, u1 + per_cm * thick, u0 + per_cm * thick], out)
                face([l0, l1, i1, i0], [u0 - per_cm * over, u1 - per_cm * over, u1, u0], -up)
                # a board's side: at a joint, and where it stands out past its neighbour
                for e, o, l, i, tt, cut, side in ((e0, o0, l0, i0, u0, t0, -d), (e1, o1, l1, i1, u1, t1, d)):
                    if 0.0 < cut < 1.0 or over > OVER or thick > PLANK:
                        face([e, o, l, i], [tt, tt - per_cm * over, tt - per_cm * over + side_v, tt + side_v], side)

        # the planks run round the corners: close the wedge between two edges' planks
        for p, outs in corners.values():
            if len(outs) != 2:
                continue
            (n1, t), (n2, _) = outs
            if (n1 - n2).length < 1e-3:
                continue
            o1, o2 = p + n1 * OVER, p + n2 * OVER
            v_cm = Vector((0.0, (m.uv(p + m.dv.normalized()) - m.uv(p)).length))
            t1, t2 = m.uv(o1), m.uv(o2)
            face([p, o1, o2], [t, t1, t2], up)
            face([o1, o2, o2 - up * PLANK, o1 - up * PLANK], [t1, t2, t2 + v_cm * PLANK, t1 + v_cm * PLANK],
                 (n1 + n2).normalized())
            face([p - up * PLANK, o1 - up * PLANK, o2 - up * PLANK], [t, t1, t2], -up)

        # --- the straps across the deck
        if owner != ctx.chunk:
            continue
        poly = [m.uv(p) for p in loop(edges)]
        signed = sum(p.x * q.y - q.x * p.y for p, q in zip(poly, poly[1:] + poly[:1]))
        if signed < 0:
            poly.reverse()
        for c in straps:
            part = clip(poly, c - STRAP_W / 2, c + STRAP_W / 2)
            if len(part) < 3:
                continue
            top = [m.point(t) + up * STRAP for t in part]
            face(top, part, up)
            for k in range(len(part)):
                p, q = part[k], part[(k + 1) % len(part)]
                a, b = m.point(p), m.point(q)
                side = (b - a).cross(up)
                if side.dot(m.point(sum(part, Vector((0.0, 0.0))) / len(part)) - a) > 0:
                    side = -side
                face([a + up * STRAP, b + up * STRAP, b, a], [p, q, q, p], side)

    print(f'crane deck: {len(built)} new faces')
    ctx.allow_open(built, "the planks' edge and the straps stand on the deck: their inner sides are the slab")
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
        normals.append(old_normals[old] if old >= 0 else Vector(me.polygons[poly_of_loop[k]].normal))
    me.normals_split_custom_set(normals)
    me.attributes.remove(me.attributes['loop_index'])
    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    print(f'chunk: {len(me.polygons)} faces, {tris} triangles after the crane decks')
