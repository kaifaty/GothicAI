"""Hero outfit pilot: the pieces of the start outfit (concept 3) over the MPFB body of the render rig.

    BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
      Blender -b <run>/body.blend -P outfit_build.py -- <run> [pieces]

<run> is a RIG=1 run of mpfb_body.py with its body.blend. Writes <run>/outfit.blend with the pieces as mesh objects
(custom property outfit_piece = name; outfit_material = the material several objects share) next to the body, and
<run>/outfit/<view>.png renders. Pieces, in this order (a later one is built over the earlier):
trousers,boots,vest,belt,flap,bracers,guards. What was built and how it is judged:
reports/content/2026-11-clothing/outfit-pilot.md. mpfb_export.py run on outfit.blend exports the pieces.

- Cloth (trousers, vest) starts from MPFB's helper-tights shell, eased off the body and draped by Blender's cloth
  simulation with the body as a collider: the folds come from the simulation, not from a brush. Where a stiff piece
  straps the cloth down it is pinned close to the leg before the simulation.
- Leather (belt, bracers, straps, guards, cuffs) is a Tube: a clean grid on rays from the limb's axis out to the
  surface under it, plus a gap. Outlines are carved along a curve (carve), not along whole faces.
- Laces, buckles and studs are small geometry; the boots are MPFB's helper sock with a flat welted sole.
Units: Blender metres of the MPFB basemesh (body height 1.742 m before the export's scale).
"""
import bpy, bmesh, os, sys, json, math
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

argv = sys.argv[sys.argv.index('--') + 1:]
RUN = argv[0]
PIECES = argv[1].split(',') if len(argv) > 1 else ['trousers', 'boots']
OUT = os.path.join(RUN, 'outfit'); os.makedirs(OUT, exist_ok=True)

body = bpy.data.objects['Human']
rig = bpy.data.objects['Human.rig']
kb = body.data.shape_keys.key_blocks
def keyco(k):
    a = np.zeros(len(body.data.vertices) * 3); k.data.foreach_get('co', a); return a.reshape(-1, 3)
base = keyco(kb[0]); CO = base.copy()
for k in kb[1:]:
    if k.value and not k.mute:
        CO += k.value * (keyco(k) - base)
def group(name, cut=0.5):
    gi = body.vertex_groups[name].index
    return np.array([v.index for v in body.data.vertices if any(g.group == gi and g.weight > cut for g in v.groups)])
BODY = group('body')
def joint(name):
    return CO[group(name)].mean(0)
KNEE = {s: joint(f'joint-{s}-knee') for s in 'lr'}
ANKLE = {s: joint(f'joint-{s}-ankle') for s in 'lr'}
HIP_Z = joint('joint-l-upper-leg')[2]

def new_object(name, verts, faces, colour):
    me = bpy.data.meshes.new(name); me.from_pydata([tuple(v) for v in verts], [], [tuple(f) for f in faces]); me.update()
    ob = bpy.data.objects.new(name, me); bpy.context.scene.collection.objects.link(ob)
    ob['outfit_piece'] = name
    mat = bpy.data.materials.new(name); mat.diffuse_color = (*colour, 1.0); me.materials.append(mat)
    for p in me.polygons: p.use_smooth = True
    return ob

def shell(centre_ok):
    """faces of helper-tights whose centre passes centre_ok, as a separate mesh in the shaped coordinates"""
    tights = set(group('helper-tights').tolist())
    faces = [list(p.vertices) for p in body.data.polygons
             if all(v in tights for v in p.vertices) and centre_ok(CO[list(p.vertices)].mean(0))]
    used = sorted({v for f in faces for v in f}); remap = {v: i for i, v in enumerate(used)}
    return CO[used], [[remap[v] for v in f] for f in faces], np.array(used)

def srgb(h):
    c = np.array([int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)])
    return tuple(np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4))

def collider_on():
    if not any(m.type == 'COLLISION' for m in body.modifiers):
        body.modifiers.new('collide', 'COLLISION')
    body.collision.thickness_outer = 0.004; body.collision.thickness_inner = 0.002
    body.collision.cloth_friction = 5.0

def simulate(ob, frames, pin, setup):
    """cloth simulation by stepping frames; pin: vertex weights (1 pinned); returns the draped coordinates"""
    vg = ob.vertex_groups.new(name='pin')
    for i, w in enumerate(pin):
        if w > 0: vg.add([i], float(w), 'REPLACE')
    cl = ob.modifiers.new('cloth', 'CLOTH'); s = cl.settings
    s.vertex_group_mass = 'pin'
    s.quality = 12; s.time_scale = 1.0
    s.mass = 0.35; s.air_damping = 2.0
    s.tension_stiffness = 20; s.compression_stiffness = 20; s.shear_stiffness = 8; s.bending_stiffness = 1.5
    s.tension_damping = 5; s.compression_damping = 5; s.shear_damping = 5; s.bending_damping = 0.5
    cl.collision_settings.collision_quality = 4; cl.collision_settings.distance_min = 0.003
    cl.collision_settings.use_self_collision = True; cl.collision_settings.self_distance_min = 0.003
    setup(s, cl)
    sc = bpy.context.scene; sc.frame_start = 1; sc.frame_end = frames
    cl.point_cache.frame_start = 1; cl.point_cache.frame_end = frames
    for f in range(1, frames + 1):
        sc.frame_set(f)
    dg = bpy.context.evaluated_depsgraph_get()
    me = ob.evaluated_get(dg).to_mesh()
    out = np.array([v.co for v in me.vertices]); ob.evaluated_get(dg).to_mesh_clear()
    ob.modifiers.remove(cl); sc.frame_set(1)
    return out

def set_coords(ob, co):
    ob.data.vertices.foreach_set('co', np.asarray(co, np.float32).reshape(-1)); ob.data.update()

def subdivide(ob, cuts=1):
    bm = bmesh.new(); bm.from_mesh(ob.data)
    bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=cuts, use_grid_fill=True, smooth=0.5)
    bm.to_mesh(ob.data); bm.free(); ob.data.update()

def trim(ob, top, bottom):
    """clean straight edges: the shell's faces cut by the planes z = top and z = bottom"""
    bm = bmesh.new(); bm.from_mesh(ob.data)
    for z, nz in ((top, 1.0), (bottom, -1.0)):
        bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=(0, 0, z),
                               plane_no=(0, 0, nz), clear_outer=True)
    bm.to_mesh(ob.data); bm.free(); ob.data.update()

def normals_of(ob):
    ob.data.update()
    return np.array([v.normal for v in ob.data.vertices])

def leg_side(p): return 'l' if p[0] > 0 else 'r'

# --- trousers: loose linen, waist at the belt, gathered into the boots at mid-shin ------------------------------
HEM_Z = float(os.environ.get('HEM_Z', 0.25))      # the boots' tops hide the hem
WAIST_Z = float(os.environ.get('WAIST_Z', 1.02))
EASE = float(os.environ.get('EASE', 0.020))         # how far the cloth starts off the thigh and the shin, m
SNUG_Z = max(KNEE['l'][2], KNEE['r'][2]) + float(os.environ.get('SNUG', 0.075))   # the knee pad's upper edge

def build_trousers():
    verts, faces, _ = shell(lambda c: HEM_Z <= c[2] <= WAIST_Z and abs(c[0]) < 0.32)
    ob = new_object('trousers', verts, faces, srgb('#3b352a'))
    trim(ob, WAIST_Z, HEM_Z)
    subdivide(ob, 1)
    co = np.array([v.co for v in ob.data.vertices]); n = normals_of(ob)
    z = co[:, 2]
    # ease: none at the waist, full on the thigh and the shin, less at the hem that gathers into the boot
    t_waist = np.clip((WAIST_Z - z) / 0.22, 0, 1)    # close at the waist: the vest and the belt go over it
    t_hem = np.clip((z - HEM_Z) / 0.08, 0.4, 1)
    ease = EASE * t_waist * t_hem
    # from the knee pad down the cloth lies close on the leg, strapped under the pad and the shin guard: the loose
    # thigh blouses over that line
    snug = z < SNUG_Z
    ease = np.where(snug, 0.006, ease * np.clip((z - SNUG_Z) / 0.05, 0.3, 1))
    co = co + n * ease[:, None]
    # the hem: drawn in towards the shin's axis (the boot shaft holds it)
    pin = np.zeros(len(co))
    for s in 'lr':
        axis_lo, axis_hi = ANKLE[s], KNEE[s]
        on = (np.sign(co[:, 0]) == (1 if s == 'l' else -1)) & (z < HEM_Z + 0.025)
        for i in np.where(on)[0]:
            t = (co[i, 2] - axis_lo[2]) / (axis_hi[2] - axis_lo[2])
            c = axis_lo + t * (axis_hi - axis_lo)
            d = co[i] - c; d[2] = 0
            co[i, :2] = c[:2] + d[:2] * 0.92
            pin[i] = 1.0
    pin[z > WAIST_Z - 0.06] = 1.0
    pin[snug] = 1.0
    set_coords(ob, co)
    def loose(s, cl):
        # the cloth is cut longer and wider than the eased tube: it buckles into folds and blouses over the boots
        s.shrink_min = -float(os.environ.get('GROW', 0.06)); s.bending_stiffness = 0.6
    draped = simulate(ob, int(os.environ.get('FRAMES', 90)), pin, loose)
    set_coords(ob, draped)
    sol = ob.modifiers.new('thick', 'SOLIDIFY'); sol.thickness = 0.003; sol.offset = -1.0
    ob['covers'] = [HEM_Z, WAIST_Z - 0.025]        # the skin under it is left out of the body (mpfb_export.py)
    return ob

# --- shared tools of the pieces -----------------------------------------------------------------------------------

TILE = 0.35                                    # metres of material per UV unit (mpfb_export.py, outfit_tiles.py)

def carve(ob, f=None, values=None):
    """drop the part of the mesh where f(point) < 0 (or the vertex's value is); the new edge follows f = 0: edges are
    split at the crossing, so the outline is the curve itself, not the stair of whole faces. A crossed face is cut
    into triangles first: a triangle is crossed along one line only"""
    bm = bmesh.new(); bm.from_mesh(ob.data)
    val = {v: float(values[v.index]) if values is not None else float(f(np.array(v.co))) for v in bm.verts}
    crossed = [fc for fc in bm.faces if min(val[v] for v in fc.verts) < 0 < max(val[v] for v in fc.verts)]
    bmesh.ops.triangulate(bm, faces=crossed)
    zero = set()
    for e in list(bm.edges):
        a, b = e.verts
        if val[a] * val[b] < 0:
            _, nv = bmesh.utils.edge_split(e, a, val[a] / (val[a] - val[b]))
            val[nv] = 0.0; zero.add(nv)
    for face in list(bm.faces):
        zs = [v for v in face.verts if v in zero]
        if len(zs) == 2:
            try: bmesh.utils.face_split(face, zs[0], zs[1])
            except ValueError: pass                    # neighbours on the face: nothing to split
    dead = [fc for fc in bm.faces if sum(val[v] for v in fc.verts) < 0]
    bmesh.ops.delete(bm, geom=dead, context='FACES')
    bm.to_mesh(ob.data); bm.free(); ob.data.update()

def keep_largest(ob):
    """drop the scraps a cut leaves: every island of the mesh but the largest"""
    bm = bmesh.new(); bm.from_mesh(ob.data)
    seen, islands = set(), []
    for f in bm.faces:
        if f in seen: continue
        isl, todo = [], [f]; seen.add(f)
        while todo:
            g = todo.pop(); isl.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h not in seen: seen.add(h); todo.append(h)
        islands.append(isl)
    islands.sort(key=len)
    bmesh.ops.delete(bm, geom=[f for isl in islands[:-1] for f in isl], context='FACES')
    bm.to_mesh(ob.data); bm.free(); ob.data.update()

def cut(ob, point, normal, where=None):
    """drop what lies on the normal's side of a plane; where(point) limits the part of the mesh the plane cuts"""
    p0, n = np.array(point, float), np.array(normal, float); n /= np.linalg.norm(n)
    carve(ob, lambda c: 1.0 if where is not None and not where(c) else -float((c - p0) @ n))

def surface(names, skin=True, side=0):
    """the outer surface to build over: the skin and the pieces named; side +1/-1 - that leg's half only, so a ray
    from outside one shin never lands on the other"""
    pv, pf = [], []
    if skin:
        bodyset = set(BODY.tolist()); pv = [tuple(c) for c in CO]
        pf = [list(p.vertices) for p in body.data.polygons if all(v in bodyset for v in p.vertices)]
    dg = bpy.context.evaluated_depsgraph_get()
    for nm in names:
        o = bpy.data.objects.get(nm)
        if o is None: continue
        pm = o.evaluated_get(dg).to_mesh(); M = o.matrix_world; b = len(pv)
        pv += [tuple(M @ v.co) for v in pm.vertices]; pf += [[b + i for i in p.vertices] for p in pm.polygons]
        o.evaluated_get(dg).to_mesh_clear()
    if side:
        pf = [f for f in pf if np.sign(np.mean([pv[i][0] for i in f])) == side]
    return BVHTree.FromPolygons(pv, pf)

def frame(u):
    """two unit vectors across the axis u: the first as near the character's front (-Y) as it gets"""
    f = np.array([0.0, -1.0, 0.0]); f = f - u * (u @ f)
    if np.linalg.norm(f) < 1e-3: f = np.array([1.0, 0.0, 0.0]) - u * u[0]
    f /= np.linalg.norm(f)
    return f, np.cross(u, f)

class Tube:
    """a band of leather round a limb or the waist: a clean grid of quads, each vertex on a ray from the axis out to
    the surface it lies over, plus a gap. Leather is stiff: the radii are smoothed and never go under the surface."""
    def __init__(self, a, b, t0, t1, under=(), gap=0.004, arc=(-180.0, 180.0), n=32, rows=6, cap=0.14, side=0,
                 smooth=2, skin=True, gap_fn=None):
        a, b = np.array(a, float), np.array(b, float)
        self.a, self.L = a, np.linalg.norm(b - a); self.u = (b - a) / self.L
        self.f, self.s = frame(self.u)
        self.full = arc[1] - arc[0] >= 359.9
        self.th = np.radians(np.linspace(arc[0], arc[1], n, endpoint=False) if self.full else np.linspace(arc[0], arc[1], n + 1))
        self.t = np.linspace(t0, t1, rows + 1)
        bvh = surface(under, skin, side)
        R = np.full((len(self.t), len(self.th)), np.nan)
        for i, t in enumerate(self.t):
            c = a + self.u * (t * self.L)
            for j, th in enumerate(self.th):
                d = math.cos(th) * self.f + math.sin(th) * self.s
                hit = bvh.ray_cast(Vector(c + d * cap), Vector(-d), cap)
                if hit[0] is not None: R[i, j] = cap - hit[3]
        if np.isnan(R).all(): raise RuntimeError('a tube found no surface under it')
        R = np.where(np.isnan(R), np.nanmean(R), R)
        raw = R.copy()
        for _ in range(smooth):
            left, right = (np.roll(R, 1, 1), np.roll(R, -1, 1)) if self.full else \
                (np.c_[R[:, :1], R[:, :-1]], np.c_[R[:, 1:], R[:, -1:]])
            R = 0.25 * left + 0.5 * R + 0.25 * right
            R = 0.25 * np.r_[R[:1], R[:-1]] + 0.5 * R + 0.25 * np.r_[R[1:], R[-1:]]
        R = np.maximum(R, raw)
        for i, t in enumerate(self.t):
            for j, th in enumerate(self.th):
                R[i, j] += gap if gap_fn is None else gap_fn(t, th)
        self.R = R

    def point(self, i, j, lift=0.0):
        d = math.cos(self.th[j]) * self.f + math.sin(self.th[j]) * self.s
        return self.a + self.u * (self.t[i] * self.L) + d * (self.R[i, j] + lift)

    def normal(self, j):
        return math.cos(self.th[j]) * self.f + math.sin(self.th[j]) * self.s

    def param(self, p):
        """(t, angle in degrees) of a point: for outlines carved in the band's own coordinates"""
        q = np.asarray(p, float) - self.a; t = (q @ self.u) / self.L
        return t, math.degrees(math.atan2(q @ self.s, q @ self.f))

    def build(self, name, colour, material=None):
        nt, nth = len(self.t), len(self.th)
        verts = [self.point(i, j) for i in range(nt) for j in range(nth)]
        cols = nth if self.full else nth - 1
        faces = [[i * nth + j, i * nth + (j + 1) % nth, (i + 1) * nth + (j + 1) % nth, (i + 1) * nth + j]
                 for i in range(nt - 1) for j in range(cols)]
        v0, v1, v3 = (np.array(verts[k]) for k in (faces[0][0], faces[0][1], faces[0][3]))
        if np.cross(v1 - v0, v3 - v0) @ self.normal(0) < 0: faces = [f[::-1] for f in faces]
        ob = new_object(name, verts, faces, colour)
        if material: ob['outfit_material'] = material
        rm = float(self.R.mean()); uvl = ob.data.uv_layers.new(name='uv')
        for p in ob.data.polygons:
            first = min(p.vertices) % nth                      # the seam column: its far side must not wrap to 0
            for l in p.loop_indices:
                i, j = divmod(ob.data.loops[l].vertex_index, nth)
                if self.full and j < first: j += nth
                ang = self.th[j % nth] + (2 * math.pi if j >= nth else 0.0)
                uvl.data[l].uv = (ang * rm / TILE, self.t[i] * self.L / TILE)
        return ob

def thick(ob, t):
    sol = ob.modifiers.new('thick', 'SOLIDIFY'); sol.thickness = t; sol.offset = -1.0; sol.use_even_offset = True

def weigh(ob, weights):
    """rigid weights of a stiff piece (rig bone -> share) instead of the nearest skin's (mpfb_export.py)"""
    for bone, w in weights.items():
        ob.vertex_groups.new(name=bone).add(list(range(len(ob.data.vertices))), w, 'REPLACE')

STUDS = []                                     # (point, outward normal, radius) of every iron stud of the outfit

def stud(p, n, r=0.006):
    STUDS.append((np.array(p, float), np.array(n, float) / np.linalg.norm(n), r))

def build_studs():
    """one mesh of all the studs: low domes, 8 sides"""
    verts, faces = [], []
    for p, n, r in STUDS:
        f, s = frame(n); b = len(verts); rings = ((0.0, 1.0), (0.55, 0.85), (0.9, 0.45))
        for h, k in rings:
            verts += [p + n * (h * r * 0.7) + (math.cos(a) * f + math.sin(a) * s) * (k * r)
                      for a in np.linspace(0, 2 * math.pi, 8, endpoint=False)]
        verts.append(p + n * (r * 0.7))
        for q in range(len(rings) - 1):
            faces += [[b + q * 8 + k, b + q * 8 + (k + 1) % 8, b + (q + 1) * 8 + (k + 1) % 8, b + (q + 1) * 8 + k] for k in range(8)]
        faces += [[b + 16 + k, b + 16 + (k + 1) % 8, b + 24] for k in range(8)]
    if not verts: return None
    ob = new_object('studs', verts, faces, srgb('#6a6862')); ob['outfit_material'] = 'studs'
    v0, v1, v2 = (np.array(verts[k]) for k in faces[0][:3])
    if np.cross(v1 - v0, v2 - v0) @ STUDS[0][1] < 0:
        ob.data.flip_normals()
    return ob

def drape_over(cloth_name, pieces, axis_lo, axis_hi, above, gap=0.004):
    """loose cloth that sags onto a strapped piece lies over it, not through it: its vertices above the height
    `above` (the cloth below is strapped under the pieces) go out along the limb's radius past the pieces' shell"""
    cloth = bpy.data.objects.get(cloth_name)
    if cloth is None: return
    bvh = surface(pieces, skin=False)
    co = np.array([v.co for v in cloth.data.vertices])
    a, b = np.array(axis_lo), np.array(axis_hi); ab = b - a; L2 = ab @ ab
    for i in np.where((np.sign(co[:, 0]) == np.sign(a[0])) & (co[:, 2] > above))[0]:
        t = (co[i] - a) @ ab / L2
        if not 0.5 <= t <= 1.5: continue
        c = a + t * ab; r = co[i] - c; rl = np.linalg.norm(r)
        if rl < 1e-6 or rl > 0.20: continue
        hit = bvh.ray_cast(Vector(c + r / rl * 0.20), Vector(-r / rl), 0.20)     # the shell's outer side
        if hit[0] is not None and rl < 0.20 - hit[3] + gap:
            co[i] = c + r / rl * (0.20 - hit[3] + gap)
    set_coords(cloth, co)

def box(name, centre, x, y, z, size, colour, material):
    """a small block with its axes x, y, z (unit vectors) and half sizes"""
    c = np.array(centre, float); hx, hy, hz = size
    verts = [c + sx * hx * np.array(x) + sy * hy * np.array(y) + sz * hz * np.array(z)
             for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    faces = [[0, 1, 3, 2], [4, 6, 7, 5], [0, 4, 5, 1], [2, 3, 7, 6], [0, 2, 6, 4], [1, 5, 7, 3]]
    ob = new_object(name, verts, faces, colour); ob['outfit_material'] = material
    for p in ob.data.polygons: p.use_smooth = False
    bm = bmesh.new(); bm.from_mesh(ob.data); bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:]); bm.to_mesh(ob.data); bm.free()
    return ob

# --- vest: sleeveless coarse wool to the belt, a laced slit at the chest ------------------------------------------
VEST_HEM = float(os.environ.get('VEST_HEM', 0.992))      # under the belt
NECK = joint('joint-neck'); NECK_Z = NECK[2]
ARM_BONES = ('upperarm', 'lowerarm', 'hand', 'thumb', 'index', 'middle', 'ring', 'pinky')
V_BOTTOM = float(os.environ.get('V_BOTTOM', 1.30)); V_SLOPE = float(os.environ.get('V_SLOPE', 3.6))
ARMHOLE = float(os.environ.get('ARMHOLE', 0.35))         # the arm bones' share of a skin point at the armhole's edge

def arm_weight():
    w = np.zeros(len(CO))
    gis = {g.index for g in body.vertex_groups if g.name.split('_')[0] in ARM_BONES}
    for v in body.data.vertices:
        w[v.index] = sum(g.weight for g in v.groups if g.group in gis)
    return w

def build_vest():
    armw = arm_weight()
    tights = set(group('helper-tights').tolist())
    faces = [list(p.vertices) for p in body.data.polygons
             if all(v in tights for v in p.vertices) and VEST_HEM - 0.03 <= CO[list(p.vertices)].mean(0)[2] <= NECK_Z + 0.05
             and armw[list(p.vertices)].min() < ARMHOLE]
    used = sorted({v for f in faces for v in f}); remap = {v: i for i, v in enumerate(used)}
    ob = new_object('vest', CO[used], [[remap[v] for v in f] for f in faces], srgb('#282523'))
    # the armholes: where the skin turns from the trunk's to the arm's (the vest's shoulder ends on the deltoid)
    carve(ob, values=ARMHOLE - armw[used]); keep_largest(ob)
    subdivide(ob, 1)
    cut(ob, (0, 0, VEST_HEM), (0, 0, -1))
    # the neck hole: a capsule along the neck; the slit: a narrow V from the hole down the chest
    R = float(os.environ.get('NECK_R', 0.088)); base = np.array(NECK) - np.array([0, 0.0, 0.035])
    def neckline(c):
        q = c - base
        q[2] -= 0.07 * min(max((c[1] - NECK[1]) / 0.08, 0.0), 1.0)   # the back of the collar sits higher than the front
        q[2] = min(q[2], 0.0)                               # below the capsule's lower end: the distance to the end
        hole = np.linalg.norm(q) - R
        slit = (abs(c[0]) * V_SLOPE - (c[2] - V_BOTTOM)) if c[1] < NECK[1] else 1.0
        return min(hole, slit)
    carve(ob, neckline)
    co = np.array([v.co for v in ob.data.vertices]); n = normals_of(ob)
    z = co[:, 2]
    ease = np.where(z > 1.36, 0.004, 0.004 + 0.020 * np.clip((1.36 - z) / 0.30, 0, 1))
    ease *= np.clip((z - VEST_HEM) / 0.05, 0.25, 1)         # close at the belt
    co = co + n * ease[:, None]
    pin = ((z > 1.38) | (z < VEST_HEM + 0.012)).astype(float)
    set_coords(ob, co)
    def wool(s, cl):
        s.shrink_min = -float(os.environ.get('VEST_GROW', 0.03)); s.bending_stiffness = 2.5; s.mass = 0.4
    draped = simulate(ob, int(os.environ.get('FRAMES', 90)), pin, wool)
    set_coords(ob, draped)
    build_laces(ob)
    sol = ob.modifiers.new('thick', 'SOLIDIFY'); sol.thickness = 0.003; sol.offset = -1.0
    return ob

def build_laces(vest):
    """the lacing of the slit: rawhide thongs crossing between the slit's edges"""
    bm = bmesh.new(); bm.from_mesh(vest.data)
    edge = {1: [], -1: []}
    for v in bm.verts:
        if v.is_boundary and v.co.y < NECK[1] and abs(v.co.x) < 0.07 and V_BOTTOM - 0.01 < v.co.z < NECK_Z - 0.02:
            edge[1 if v.co.x > 0 else -1].append(np.array(v.co))
    bm.free()
    if not edge[1] or not edge[-1]: return
    def at(sgn, z):
        p = min(edge[sgn], key=lambda c: abs(c[2] - z)).copy(); p[0] += sgn * 0.007; p[1] -= 0.0035
        return p
    top = min(max(c[2] for c in edge[1]), max(c[2] for c in edge[-1])) - 0.012
    zs = np.linspace(V_BOTTOM + 0.035, top, 5)
    k = 0
    for i in range(len(zs) - 1):
        for sgn in (1, -1):
            p, q = at(sgn, zs[i]), at(-sgn, zs[i + 1]); d = q - p; L = np.linalg.norm(d); d /= L
            out = np.array([0.0, -1.0, 0.0]); out -= d * (out @ d); out /= np.linalg.norm(out)
            lift = out * (0.0015 if sgn > 0 else 0.0035)            # one thong over the other where they cross
            box(f'lace{k}', (p + q) / 2 + lift, d, np.cross(out, d), out, (L / 2 + 0.004, 0.0028, 0.0012),
                srgb('#8a7a62'), 'laces')
            k += 1

# --- boots: soft leather to mid-shin with a turned-down suede cuff ------------------------------------------------
BOOT_TOP = float(os.environ.get('BOOT_TOP', 0.31))
CUFF = float(os.environ.get('CUFF', 0.075))   # height of the turned-down cuff

def build_boots():
    """soft leather boots to mid-shin: MPFB's helper sock (a foot without toes) and the shin's shell, off the skin by
    the leather and what is tucked in, the sole flat on the ground with a welt round it"""
    verts, faces, _ = shell(lambda c: c[2] <= BOOT_TOP + 0.03)
    ob = new_object('boots', verts, faces, srgb('#2a2018'))
    cut(ob, (0, 0, BOOT_TOP), (0, 0, 1))
    subdivide(ob, 1)
    co = np.array([v.co for v in ob.data.vertices]); n = normals_of(ob); z = co[:, 2].copy()
    co += n * (0.006 + 0.003 * np.clip((z - 0.10) / 0.10, 0, 1))[:, None]      # the shaft a little looser than the foot
    flat = n.copy(); flat[:, 2] = 0; flat /= np.maximum(np.linalg.norm(flat, axis=1, keepdims=True), 1e-9)
    welt = np.clip((0.022 - z) / 0.012, 0, 1)                                    # the sole's edge stands out
    co += flat * (0.005 * welt)[:, None]
    co[:, 2] = np.maximum(co[:, 2], 0.0)                                         # the sole on the ground
    set_coords(ob, co)
    push_off_skin(ob, HEM_Z - 0.03, 0.004, ['trousers'])   # outside the trousers tucked into it
    ob['covers'] = [-1.0, BOOT_TOP - 0.03]
    # the turned-down cuff: a suede band over the shaft's top, flaring to its lower edge
    for s in 'lr':
        ankle, knee = np.array(ANKLE[s]), np.array(KNEE[s]); L = np.linalg.norm(knee - ankle)
        t0 = (BOOT_TOP - CUFF - ankle[2]) / (knee[2] - ankle[2]); t1 = (BOOT_TOP + 0.012 - ankle[2]) / (knee[2] - ankle[2])
        tb = Tube(ankle, knee, t0, t1, under=['boots'], n=24, rows=3, cap=0.12, side=1 if s == 'l' else -1, smooth=3,
                  skin=False, gap_fn=lambda t, th: 0.005 + 0.012 * (t1 - t) / (t1 - t0))
        cf = tb.build(f'cuff_{s}', srgb('#5a4330'), 'cuffs'); thick(cf, 0.004); weigh(cf, {f'calf_{s}': 1.0})
    return ob

def push_off_skin(ob, zmin, gap, under=()):
    """vertices above zmin that sit inside the skin (and the pieces named) or nearer than gap go out along their
    horizontal normal"""
    bvh = surface(list(under))
    co = np.array([v.co for v in ob.data.vertices]); n = normals_of(ob)
    n[:, 2] = 0; n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    for i in np.where(co[:, 2] > zmin)[0]:
        # the skin's surface along the normal through the vertex, from well inside the leg
        far, o = None, Vector(co[i] - n[i] * 0.08)         # the outermost surface on the way out
        while True:
            hit = bvh.ray_cast(o, Vector(n[i]), 0.16)
            if hit[0] is None or (hit[0] - Vector(co[i] - n[i] * 0.08)).length > 0.16: break
            far = hit[0]; o = hit[0] + Vector(n[i]) * 1e-4
        if far is None: continue
        skin = (np.array(far) - co[i]) @ n[i]              # > 0: the surface is outside the vertex
        if skin > -gap: co[i] += n[i] * (skin + gap)
    set_coords(ob, co)

# --- leather over the cloth: belt with buckle, front flap, bracers, knee pads, shin guards -------------------------
BELT = (float(os.environ.get('BELT_LO', 0.985)), float(os.environ.get('BELT_HI', 1.035)))
LEATHER = {'belt': '#3a2a1e', 'flap': '#33261b', 'bracer': '#2b2420', 'guards': '#231c17', 'straps': '#3a2a1e'}

def build_belt():
    lo, hi = BELT
    band = CO[BODY]; band = band[(band[:, 2] > lo) & (band[:, 2] < hi) & (np.abs(band[:, 0]) < 0.3)]
    cy = (band[:, 1].min() + band[:, 1].max()) / 2
    tb = Tube((0, cy, lo), (0, cy, hi), 0.0, 1.0, under=['vest', 'trousers'], gap=0.0035, n=64, rows=3, cap=0.32, smooth=3)
    ob = tb.build('belt', srgb(LEATHER['belt'])); thick(ob, 0.004)
    # the buckle: an iron frame at the front with its prong, and the belt's studs round the back
    j0 = int(np.argmin(np.abs(tb.th))); out = tb.normal(j0); side = np.cross(tb.u, out)
    c = (tb.point(1, j0) + tb.point(2, j0)) / 2 + out * 0.004
    h, w, bar = (hi - lo) / 2 + 0.006, 0.034, 0.0045
    for k, (dx, dz, sx, sz) in enumerate(((0, h, w, bar), (0, -h, w, bar), (w, 0, bar, h + bar), (-w, 0, bar, h + bar))):
        box(f'buckle{k}', c + side * dx + tb.u * dz, side, out, tb.u, (sx, 0.003, sz), srgb('#5a5a58'), 'buckle')
    box('buckle_prong', c + out * 0.003, side, out, tb.u, (w, 0.002, 0.003), srgb('#5a5a58'), 'buckle')
    for j in range(len(tb.th)):
        if abs(math.degrees(tb.th[j])) > 35 and j % 4 == 0:
            stud((tb.point(1, j) + tb.point(2, j)) / 2, tb.normal(j), 0.005)
    return ob

FLAP_LOW = float(os.environ.get('FLAP_LOW', 0.70)); FLAP_W = float(os.environ.get('FLAP_W', 0.105))

def build_flap():
    """the leather flap hangs from the belt in front over the most forward points of the thighs, its corners round"""
    bvh = surface(['trousers', 'vest'])
    nx, nz = 13, 18
    xs = np.linspace(-FLAP_W, FLAP_W, nx); zs = np.linspace(BELT[0] + 0.012, FLAP_LOW, nz)
    front = np.zeros(nz)
    for j, z in enumerate(zs):
        ys = []
        for x in np.linspace(-FLAP_W, FLAP_W, 9):
            hit = bvh.ray_cast(Vector((x, -0.6, z)), Vector((0, 1, 0)), 1.0)
            if hit[0] is not None: ys.append(hit[0][1])
        front[j] = min(ys) if ys else -0.15
    front = np.minimum.accumulate(front)          # leather does not fold back in: it hangs over the forward-most point
    verts = [(x, front[j] - 0.007 + 0.35 * x * x, z) for j, z in enumerate(zs) for x in xs]
    faces = [[j * nx + i, j * nx + i + 1, (j + 1) * nx + i + 1, (j + 1) * nx + i] for j in range(nz - 1) for i in range(nx - 1)]
    ob = new_object('flap', verts, faces, srgb(LEATHER['flap']))
    r = 0.035                                      # the lower corners' radius
    def outline(c):
        dx, dz = abs(c[0]) - (FLAP_W - r), (FLAP_LOW + r) - c[2]
        return 1.0 if dx < 0 or dz < 0 else r - math.hypot(dx, dz) + 1e-4
    carve(ob, outline)
    uvl = ob.data.uv_layers.new(name='uv')
    for l in ob.data.loops:
        c = ob.data.vertices[l.vertex_index].co; uvl.data[l.index].uv = (c.x / TILE, c.z / TILE)
    thick(ob, 0.003)
    for sgn in (-1, 1):                            # two studs hold it to the belt
        stud((sgn * (FLAP_W - 0.025), front[0] - 0.007 + 0.35 * (FLAP_W - 0.025) ** 2, zs[0] + 0.012), (0, -1, 0), 0.0055)
    return ob

def limb_axis(a, b): return np.array(joint(a)), np.array(joint(b))

def build_bracers():
    first = None
    for s in 'lr':
        wrist, elbow = limb_axis(f'joint-{s}-hand', f'joint-{s}-elbow')
        tb = Tube(wrist, elbow, 0.10, 0.60, gap=0.0045, n=24, rows=6, cap=0.10, smooth=2)
        ob = tb.build(f'bracer_{s}', srgb(LEATHER['bracer']), 'bracer'); thick(ob, 0.004)
        first = first or ob
        for k, (t0, t1) in enumerate(((0.085, 0.135), (0.565, 0.615))):          # the light rawhide edging
            tr = Tube(wrist, elbow, t0, t1, gap=0.0068, n=24, rows=1, cap=0.10, smooth=2)
            tro = tr.build(f'bracer_trim_{s}{k}', srgb('#8a7a62'), 'bracer_trim'); thick(tro, 0.003)
        for i in (2, 4):                                                          # two rings of studs
            for j in range(0, len(tb.th), 3):
                stud(tb.point(i, j), tb.normal(j))
    return first

def build_guards():
    for s in 'lr':
        sd = 1 if s == 'l' else -1
        knee, ankle = np.array(KNEE[s]), np.array(ANKLE[s]); L = np.linalg.norm(knee - ankle)
        tz = lambda z: (z - ankle[2]) / (knee[2] - ankle[2])          # the leg's parameter at a height
        # knee pad: a domed cap over the kneecap, held by a strap round the leg
        t0, t1 = 1 - 0.060 / L, 1 + 0.075 / L; tk = (t0 + t1) / 2; half = 62.0
        dome = lambda t, th: 0.009 + 0.014 * max(0.0, 1 - ((t - tk) / (t1 - tk)) ** 2) * max(0.0, math.cos(th * 90 / math.radians(half) * math.pi / 180))
        tb = Tube(ankle, knee, t0, t1, under=['trousers'], n=18, rows=12, arc=(-half, half), cap=0.13, side=sd, smooth=3, gap_fn=dome)
        pad = tb.build(f'kneepad_{s}', srgb(LEATHER['guards']), 'guards')
        def oval(c, tb=tb, tk=tk, ht=(t1 - t0) / 2, half=half):
            t, th = tb.param(c)
            return 1.0 - (abs((t - tk) / ht) ** 2.6 + abs(th / half) ** 2.6)
        carve(pad, oval); thick(pad, 0.006); weigh(pad, {f'calf_{s}': 0.65, f'thigh_{s}': 0.35})
        st = Tube(ankle, knee, tk - 0.016 / L, tk + 0.016 / L, under=['trousers'], n=24, rows=1, cap=0.13, side=sd, smooth=2, gap=0.0045)
        so = st.build(f'kneestrap_{s}', srgb(LEATHER['straps']), 'straps'); thick(so, 0.003)
        weigh(so, {f'calf_{s}': 0.65, f'thigh_{s}': 0.35})
        drape_over('trousers', [pad.name, so.name], ankle, knee, SNUG_Z + 0.002)
        # shin guard: the front of the shin from below the knee into the boot's cuff, two straps round the calf
        g0, g1 = tz(BOOT_TOP + 0.010), 1 - 0.085 / L; half = 78.0     # from the cuff's fold up
        tg = Tube(ankle, knee, g0, g1, under=['trousers'], n=18, rows=16, arc=(-half, half), cap=0.13, side=sd, smooth=3, gap=0.012)
        g = tg.build(f'shinguard_{s}', srgb(LEATHER['guards']), 'guards')
        def plate(c, tg=tg, gm=(g0 + g1) / 2, ht=(g1 - g0) / 2, half=half):
            t, th = tg.param(c)
            return 1.0 - (abs((t - gm) / ht) ** 6 + abs(th / half) ** 6)
        carve(g, plate); thick(g, 0.005); weigh(g, {f'calf_{s}': 1.0})
        for k, t in enumerate((g0 + 0.24 * (g1 - g0), g0 + 0.76 * (g1 - g0))):
            st = Tube(ankle, knee, t - 0.012 / L, t + 0.012 / L, under=['trousers'], n=24, rows=1, cap=0.13, side=sd, smooth=2, gap=0.0045)
            so = st.build(f'shinstrap_{s}{k}', srgb(LEATHER['straps']), 'straps'); thick(so, 0.003)
            weigh(so, {f'calf_{s}': 1.0})
            jb = int(np.argmax(np.abs(st.th)))                                   # the strap's buckle behind the calf
            box(f'shinbuckle_{s}{k}', st.point(0, jb, 0.002) * 0.5 + st.point(1, jb, 0.002) * 0.5, np.cross(st.u, st.normal(jb)),
                st.normal(jb), st.u, (0.011, 0.002, 0.015), srgb('#5a5a58'), 'buckle')
        for i in (3, 6, 10, 13):                                                  # studs down both edges of the plate
            for j in (2, len(tg.th) - 3):
                stud(tg.point(i, j), tg.normal(j))
    return None

BUILD = {'trousers': build_trousers, 'boots': build_boots, 'vest': build_vest, 'belt': build_belt, 'flap': build_flap,
         'bracers': build_bracers, 'guards': build_guards}
collider_on()
built = [b for b in (BUILD[p]() for p in PIECES) if b is not None]
build_studs()
for m in body.modifiers:
    if m.type == 'COLLISION':
        body.modifiers.remove(m)

# --- renders: front, side, back, three-quarter, with the body ---------------------------------------------------
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'MATERIAL'
sc.display.shading.show_cavity = True
sc.render.resolution_x, sc.render.resolution_y = 700, 1100
skin = bpy.data.materials.new('skin_view'); skin.diffuse_color = (0.55, 0.42, 0.34, 1)
body.data.materials.clear(); body.data.materials.append(skin)
cam_d = bpy.data.cameras.new('cam'); cam_d.type = 'ORTHO'; cam_d.ortho_scale = 1.15
cam = bpy.data.objects.new('cam', cam_d); sc.collection.objects.link(cam); sc.camera = cam
centre = Vector((0, 0, 0.55))
for name, d in (('front', (0, -1, 0)), ('side', (1, 0, 0)), ('back', (0, 1, 0)), ('quarter', (0.7, -0.7, 0))):
    cam.location = centre + Vector(d) * 4
    cam.rotation_euler = (centre - cam.location).to_track_quat('-Z', 'Y').to_euler()
    sc.render.filepath = os.path.join(OUT, f'{name}.png')
    bpy.ops.render.render(write_still=True)
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(RUN, 'outfit.blend'))
print('OUTFIT', json.dumps({p.name: {'verts': len(p.data.vertices), 'faces': len(p.data.polygons)} for p in built}))
