"""Hero outfit pilot: the pieces of the start outfit (concept 3) over the MPFB body of the render rig.

    BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
      Blender -b <run>/body.blend -P outfit_build.py -- <run> [pieces]

<run> is a RIG=1 run of mpfb_body.py (work/body/rig1). Writes <run>/outfit.blend with the pieces as mesh objects
(custom property outfit_piece = name, material per piece) next to the body, and <run>/outfit/<view>.png renders.
Pieces: trousers, boots (more follow: vest, belt, bracers, guards). Plan and acceptance:
reports/content/2026-11-clothing/outfit-pilot.md.

- Cloth pieces start from MPFB's helper-tights shell, eased off the body and draped by Blender's cloth simulation
  with the body as a collider: the folds come from the simulation, not from a brush.
- Leather pieces are built over the body's surface with an offset, a thickness and a rounded shape.
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
    set_coords(ob, co)
    def loose(s, cl):
        # the cloth is cut longer and wider than the eased tube: it buckles into folds and blouses over the boots
        s.shrink_min = -float(os.environ.get('GROW', 0.06)); s.bending_stiffness = 0.6
    draped = simulate(ob, int(os.environ.get('FRAMES', 90)), pin, loose)
    set_coords(ob, draped)
    sol = ob.modifiers.new('thick', 'SOLIDIFY'); sol.thickness = 0.003; sol.offset = -1.0
    ob['covers'] = [HEM_Z, WAIST_Z - 0.025]        # the skin under it is left out of the body (mpfb_export.py)
    return ob

# --- vest: sleeveless coarse wool to the hips, laced V at the chest, under the belt -------------------------------
VEST_HEM = float(os.environ.get('VEST_HEM', 0.95))
NECK_Z = joint('joint-neck')[2]
ARM_BONES = ('upperarm', 'lowerarm', 'hand', 'thumb', 'index', 'middle', 'ring', 'pinky')

def arm_weight():
    w = np.zeros(len(CO))
    gis = {g.index for g in body.vertex_groups if g.name.split('_')[0] in ARM_BONES}
    for v in body.data.vertices:
        w[v.index] = sum(g.weight for g in v.groups if g.group in gis)
    return w

def cut(ob, point, normal, where=None):
    """bisect the mesh by a plane and drop what lies on the normal's side; where(centre) limits the faces cut"""
    bm = bmesh.new(); bm.from_mesh(ob.data)
    faces = [f for f in bm.faces if where is None or where(np.array(f.calc_center_median()))]
    geom = list({v for f in faces for v in f.verts}) + list({e for f in faces for e in f.edges}) + faces
    bmesh.ops.bisect_plane(bm, geom=geom, plane_co=point, plane_no=normal, clear_outer=True)
    bm.to_mesh(ob.data); bm.free(); ob.data.update()

def build_vest():
    armw = arm_weight()
    tights = set(group('helper-tights').tolist())
    faces = [list(p.vertices) for p in body.data.polygons
             if all(v in tights for v in p.vertices) and VEST_HEM - 0.02 <= CO[list(p.vertices)].mean(0)[2] <= NECK_Z
             and armw[list(p.vertices)].max() < 0.85]
    used = sorted({v for f in faces for v in f}); remap = {v: i for i, v in enumerate(used)}
    ob = new_object('vest', CO[used], [[remap[v] for v in f] for f in faces], srgb('#282523'))
    subdivide(ob, 1)
    trim(ob, NECK_Z, VEST_HEM)
    # the neck hole: a ring round the neck, its edge snapped onto the ring
    neck = joint('joint-neck'); R = float(os.environ.get('NECK_R', 0.085))
    bm = bmesh.new(); bm.from_mesh(ob.data)
    inside = [f for f in bm.faces if f.calc_center_median().z > 1.38 and
              math.hypot(f.calc_center_median().x - neck[0], f.calc_center_median().y - neck[1]) < R]
    bmesh.ops.delete(bm, geom=inside, context='FACES')
    for v in bm.verts:
        if v.is_boundary and v.co.z > 1.40:
            d = Vector((v.co.x - neck[0], v.co.y - neck[1], 0))
            if d.length < R * 1.6:
                d = d.normalized() * R; v.co.x, v.co.y = neck[0] + d.x, neck[1] + d.y
    bm.to_mesh(ob.data); bm.free(); ob.data.update()
    v_bottom, chest_y = float(os.environ.get('V_BOTTOM', 1.34)), -0.11
    for sgn in (1, -1):
        # the V: z - 3|x| - 3 (y - chest) > v_bottom, the back untouched
        cut(ob, (0, chest_y, v_bottom), (-3.0 * sgn, -3.0, 1.0), lambda c, sgn=sgn: sgn * c[0] >= -0.001 and c[1] < 0)
        # the armhole: from the shoulder's top down to the armpit
        cut(ob, (sgn * 0.168, 0, 1.26), (sgn, 0, -0.12), lambda c: c[2] > 1.24)
    co = np.array([v.co for v in ob.data.vertices]); n = normals_of(ob)
    z = co[:, 2]
    ease = np.where(z > 1.36, 0.004, 0.004 + 0.022 * np.clip((1.36 - z) / 0.30, 0, 1))
    co = co + n * ease[:, None]
    pin = (z > 1.38).astype(float)
    set_coords(ob, co)
    def wool(s, cl):
        s.shrink_min = -float(os.environ.get('VEST_GROW', 0.03)); s.bending_stiffness = 2.5; s.mass = 0.4
    draped = simulate(ob, int(os.environ.get('FRAMES', 90)), pin, wool)
    set_coords(ob, draped)
    sol = ob.modifiers.new('thick', 'SOLIDIFY'); sol.thickness = 0.003; sol.offset = -1.0
    return ob

# --- boots: MPFB's CC0 shoes03 (leather boots to mid-calf), fitted and weighted by MPFB --------------------------
from bl_ext.user_default.mpfb.services import HumanService, LocationService

def build_boots():
    path = LocationService.get_user_data(os.path.join('clothes', 'shoes03', 'shoes03.mhclo'))
    ob = HumanService.add_mhclo_asset(path, body, subdiv_levels=0)
    ob.name = 'boots'; ob['outfit_piece'] = 'boots'
    mat = bpy.data.materials.new('boots'); mat.diffuse_color = (*srgb('#2a2018'), 1.0)
    ob.data.materials.clear(); ob.data.materials.append(mat)
    # the shaft up to mid-shin, a little wider at the top
    co = np.array([v.co for v in ob.data.vertices]); top = co[:, 2].max()
    for s in 'lr':
        on = np.sign(co[:, 0]) == (1 if s == 'l' else -1)
        hi = on & (co[:, 2] > SHAFT_FROM)
        t = (co[hi, 2] - SHAFT_FROM) / (top - SHAFT_FROM)
        z_new = SHAFT_FROM + t * (BOOT_TOP - SHAFT_FROM)
        for k, i in enumerate(np.where(hi)[0]):
            a = ANKLE[s] + (KNEE[s] - ANKLE[s]) * ((z_new[k] - ANKLE[s][2]) / (KNEE[s][2] - ANKLE[s][2]))
            a0 = ANKLE[s] + (KNEE[s] - ANKLE[s]) * ((co[i, 2] - ANKLE[s][2]) / (KNEE[s][2] - ANKLE[s][2]))
            d = co[i, :2] - a0[:2]
            co[i, :2] = a[:2] + d * (1.0 + 0.25 * t[k])
            co[i, 2] = z_new[k]
    set_coords(ob, co)
    push_off_skin(ob, SHAFT_FROM, 0.007)              # the stretched shaft stays outside the calf
    ob['covers'] = [-1.0, BOOT_TOP - 0.03]
    build_cuffs(ob)
    return ob

def push_off_skin(ob, zmin, gap):
    """vertices above zmin that sit inside the skin or nearer than gap go out along their horizontal normal"""
    me = body.data; bodyset = set(BODY.tolist())
    tris = [[p.vertices[0], p.vertices[k], p.vertices[k + 1]] for p in me.polygons
            if all(v in bodyset for v in p.vertices) for k in range(1, len(p.vertices) - 1)]
    bvh = BVHTree.FromPolygons([tuple(c) for c in CO], tris)
    co = np.array([v.co for v in ob.data.vertices]); n = normals_of(ob)
    n[:, 2] = 0; n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    for i in np.where(co[:, 2] > zmin)[0]:
        # the skin's surface along the normal through the vertex, from well inside the leg
        hit = bvh.ray_cast(Vector(co[i] - n[i] * 0.08), Vector(n[i]), 0.16)
        if hit[0] is None: continue
        skin = (np.array(hit[0]) - co[i]) @ n[i]           # > 0: the skin is outside the vertex
        if skin > -gap: co[i] += n[i] * (skin + gap)
    set_coords(ob, co)

SHAFT_FROM = 0.10          # the boot keeps its foot below this height and stretches its shaft above it
BOOT_TOP = float(os.environ.get('BOOT_TOP', 0.31))
CUFF = float(os.environ.get('CUFF', 0.075))   # height of the turned-down cuff

def build_cuffs(boots):
    """the turned-down cuff: the shaft's top band, eased off the shaft and flared at its lower edge"""
    me = boots.data
    co = np.array([v.co for v in me.vertices])
    keep = co[:, 2] >= BOOT_TOP - CUFF - 0.004
    faces = [list(p.vertices) for p in me.polygons if all(keep[v] for v in p.vertices)]
    used = sorted({v for f in faces for v in f}); remap = {v: i for i, v in enumerate(used)}
    ob = new_object('cuffs', co[used], [[remap[v] for v in f] for f in faces], srgb('#5a4330'))
    trim(ob, BOOT_TOP + 0.01, BOOT_TOP - CUFF)
    c = np.array([v.co for v in ob.data.vertices]); n = normals_of(ob)
    t = np.clip((BOOT_TOP - c[:, 2]) / CUFF, 0, 1)          # 0 at the fold, 1 at the cuff's lower edge
    c = c + n * (0.006 + 0.012 * t)[:, None]
    c[:, 2] += 0.004
    set_coords(ob, c)
    sol = ob.modifiers.new('thick', 'SOLIDIFY'); sol.thickness = 0.004; sol.offset = -1.0
    ob.parent = boots.parent
    return ob

# --- leather over the cloth: belt with buckle, front flap, bracers, knee pads, shin guards -------------------------
from mathutils.bvhtree import BVHTree

def bvh_of(names):
    dg = bpy.context.evaluated_depsgraph_get(); pv, pf = [], []
    for nm in names:
        o = bpy.data.objects.get(nm)
        if o is None: continue
        pm = o.evaluated_get(dg).to_mesh(); M = o.matrix_world; b = len(pv)
        pv += [tuple(M @ v.co) for v in pm.vertices]; pf += [[b + i for i in p.vertices] for p in pm.polygons]
        o.evaluated_get(dg).to_mesh_clear()
    return BVHTree.FromPolygons(pv, pf) if pf else None

def skin_patch(name, colour, face_ok):
    """body skin faces whose centre passes face_ok(centre, normal), as a new piece"""
    me = body.data; me.update()
    bodyset = set(BODY.tolist())
    faces = []
    for p in me.polygons:
        vs = list(p.vertices)
        if not all(v in bodyset for v in vs): continue
        c = CO[vs].mean(0)
        a, b2, c2 = CO[vs[0]], CO[vs[1]], CO[vs[2]]
        nrm = np.cross(b2 - a, c2 - a); nrm /= max(np.linalg.norm(nrm), 1e-9)
        if face_ok(c, nrm): faces.append(vs)
    used = sorted({v for f in faces for v in f}); remap = {v: i for i, v in enumerate(used)}
    return new_object(name, CO[used], [[remap[v] for v in f] for f in faces], colour)

def clear_over(ob, under, gap, cap=0.06, smooth=0, flat=False):
    """push every vertex out along its normal past the pieces named in under (their outer surface) plus gap;
    flat: along the horizontal part of the normal (a band round the waist must not slide down)"""
    bvh = bvh_of(under)
    co = np.array([v.co for v in ob.data.vertices]); n = normals_of(ob)
    if flat:
        n[:, 2] = 0; n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    for i in range(len(co)):
        lift = gap
        if bvh is not None:
            far = 0.0; o = Vector(co[i] - n[i] * 0.01); d = Vector(n[i])
            while True:
                hit = bvh.ray_cast(o, d, cap)
                if hit[0] is None: break
                far = max(far, (hit[0] - Vector(co[i])).dot(d)); o = hit[0] + d * 1e-4
            lift = min(far, cap) + gap
        co[i] += n[i] * lift
    set_coords(ob, co)
    if smooth:
        bm = bmesh.new(); bm.from_mesh(ob.data)
        for _ in range(smooth):
            bmesh.ops.smooth_vert(bm, verts=bm.verts[:], factor=0.5, use_axis_x=True, use_axis_y=True, use_axis_z=True)
        bm.to_mesh(ob.data); bm.free(); ob.data.update()

def press_under(cloth_name, guard, axis_lo, axis_hi, gap=0.004):
    """the cloth under a strapped guard: its vertices outside the guard's shell along the limb drawn inside it"""
    cloth = bpy.data.objects.get(cloth_name)
    if cloth is None: return
    bvh = bvh_of([guard.name])
    co = np.array([v.co for v in cloth.data.vertices])
    a, b = np.array(axis_lo), np.array(axis_hi); ab = b - a; L2 = ab @ ab
    side = np.sign(a[0])
    for i in range(len(co)):
        if np.sign(co[i, 0]) != side: continue          # only this leg's cloth
        t = (co[i] - a) @ ab / L2
        if not 0.0 <= t <= 1.0: continue
        c = a + t * ab; r = co[i] - c
        rl = np.linalg.norm(r)
        if rl < 1e-6 or rl > 0.20: continue
        hit = bvh.ray_cast(Vector(c), Vector(r / rl), 0.30)
        if hit[0] is None: continue
        rg = (np.array(hit[0]) - c) @ (r / rl)
        if rl > rg - gap: co[i] = c + r / rl * (rg - gap)
    set_coords(cloth, co)

def thick(ob, t):
    sol = ob.modifiers.new('thick', 'SOLIDIFY'); sol.thickness = t; sol.offset = -1.0; sol.use_even_offset = True

BELT = (float(os.environ.get('BELT_LO', 0.985)), float(os.environ.get('BELT_HI', 1.04)))

def build_belt():
    lo, hi = BELT
    ob = skin_patch('belt', srgb('#3a2a1e'), lambda c, n: lo - 0.02 <= c[2] <= hi + 0.02 and abs(c[0]) < 0.30)
    trim(ob, hi, lo); subdivide(ob, 1)
    clear_over(ob, ['vest', 'trousers'], 0.003, smooth=6, flat=True)
    thick(ob, 0.004)
    # the buckle: an iron frame at the front
    co = np.array([v.co for v in ob.data.vertices]); front = co[np.argmin(co[:, 1])]
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, front[1] - 0.006, (lo + hi) / 2))
    bk = bpy.context.active_object; bk.name = 'buckle'; bk.scale = (0.055, 0.008, hi - lo + 0.01)
    bpy.ops.object.transform_apply(scale=True)
    bk['outfit_piece'] = 'buckle'
    m = bpy.data.materials.new('buckle'); m.diffuse_color = (*srgb('#4a4a48'), 1); bk.data.materials.append(m)
    return ob

FLAP_LOW = float(os.environ.get('FLAP_LOW', 0.70))

def build_flap():
    """the leather flap hangs from the belt in front, nearly flat, over the most forward points of the thighs"""
    bvh = bvh_of(['trousers', 'vest'])
    xs = np.linspace(-0.10, 0.10, 11); zs = np.linspace(BELT[0] + 0.005, FLAP_LOW, 16)
    front = np.zeros(len(zs))
    for j, z in enumerate(zs):
        ys = []
        for x in np.linspace(-0.11, 0.11, 9):
            hit = bvh.ray_cast(Vector((x, -0.6, z)), Vector((0, 1, 0)), 1.0)
            if hit[0] is not None: ys.append(hit[0][1])
        front[j] = min(ys) if ys else -0.15
    front = np.minimum.accumulate(front)          # leather does not fold back in: it hangs over the forward-most point
    verts = [(x, front[j] - 0.008 + 0.25 * x * x, z) for j, z in enumerate(zs) for x in xs]
    faces = [[j * 11 + i, j * 11 + i + 1, (j + 1) * 11 + i + 1, (j + 1) * 11 + i] for j in range(15) for i in range(10)]
    ob = new_object('flap', verts, faces, srgb('#33261b'))
    thick(ob, 0.003)
    return ob

def limb_axis(a, b): return np.array(joint(a)), np.array(joint(b))

def build_bracers():
    pieces = []
    for s in 'lr':
        wrist, elbow = limb_axis(f'joint-{s}-hand', f'joint-{s}-elbow')
        ab = elbow - wrist; L = np.linalg.norm(ab); u = ab / L
        def ok(c, n, wrist=wrist, u=u, L=L):
            t = (c - wrist) @ u / L; r = np.linalg.norm((c - wrist) - ((c - wrist) @ u) * u)
            return 0.04 <= t <= 0.62 and r < 0.07
        ob = skin_patch(f'bracer_{s}', srgb('#2b2420'), ok)
        cut(ob, tuple(wrist + u * L * 0.08), tuple(-u)); cut(ob, tuple(wrist + u * L * 0.58), tuple(u))
        subdivide(ob, 1)
        clear_over(ob, [], 0.006, smooth=2); thick(ob, 0.004)
        pieces.append(ob)
        for k, (t0, t1) in enumerate(((0.08, 0.13), (0.53, 0.58))):          # the light rawhide edging
            tr = skin_patch(f'bracer_trim_{s}{k}', srgb('#8a7a62'), ok)
            cut(tr, tuple(wrist + u * L * t0), tuple(-u)); cut(tr, tuple(wrist + u * L * t1), tuple(u))
            clear_over(tr, [], 0.0085, smooth=2); thick(tr, 0.003)
            pieces.append(tr)
    return pieces[0]

def build_guards():
    for s in 'lr':
        knee, ankle = np.array(KNEE[s]), np.array(ANKLE[s])
        ab = knee - ankle; L = np.linalg.norm(ab); u = ab / L
        fwd = np.array([0.0, -1.0, 0.0])
        # knee pad: a cap over the kneecap
        kp = knee + fwd * 0.05
        pad = skin_patch(f'kneepad_{s}', srgb('#1e1a17'),
                         lambda c, n, kp=kp: np.linalg.norm(c - kp) < 0.075 and n[1] < -0.2)
        subdivide(pad, 1)
        clear_over(pad, ['trousers'], 0.004, cap=0.05, smooth=8); thick(pad, 0.006)
        press_under('trousers', pad, ankle, knee + u * 0.2)
        # shin guard: the front half of the shin from below the knee into the boot's cuff
        lo_z, hi_z = BOOT_TOP - 0.03, knee[2] - 0.075
        def ok(c, n, ankle=ankle, u=u, s=s):
            r = (c - ankle) - ((c - ankle) @ u) * u
            return lo_z <= c[2] <= hi_z and r[1] < 0.01 and np.sign(c[0]) == (1 if s == 'l' else -1) \
                and np.linalg.norm(r) < 0.09
        g = skin_patch(f'shinguard_{s}', srgb('#241d18'), ok)
        trim(g, hi_z, lo_z); subdivide(g, 1)
        clear_over(g, [], 0.022, smooth=4); thick(g, 0.005)
        press_under('trousers', g, ankle, knee)
    return None

BUILD = {'trousers': build_trousers, 'boots': build_boots, 'vest': build_vest, 'belt': build_belt, 'flap': build_flap,
         'bracers': build_bracers, 'guards': build_guards}
collider_on()
built = [b for b in (BUILD[p]() for p in PIECES) if b is not None]
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
