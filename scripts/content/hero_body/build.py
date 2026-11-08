# blender -b -P build.py -- <glb> <HUMANS.MDH.json> <landmarks.json> <outdir> [tris] [texsize]
"""Fit the Tripo hero to HUMANS.MDH, decimate, bake maps high->low, weight to the Gothic bones."""
import bpy, bmesh, sys, os, json, math, time
import numpy as np
from mathutils import Vector, Matrix

argv = sys.argv[sys.argv.index('--') + 1:]
GLB, MDH, LMK, OUT = argv[:4]
TRIS = int(argv[4]) if len(argv) > 4 else 40000
TEX = int(argv[5]) if len(argv) > 5 else 2048
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fit import load_skeleton, landmarks_work, posed_skeleton, control_points, TPS, tz_of, g2w, S
os.makedirs(OUT, exist_ok=True)
log = {}
t0 = time.time()
def stamp(k):
    log[k] = round(time.time() - t0, 1); print('==', k, log[k], flush=True)

def b2w(co):   # Blender (m, Z up) -> W (cm, glTF axes)
    return np.c_[co[:, 0], co[:, 2], -co[:, 1]] * 100
def w2b(p):
    return np.c_[p[:, 0], -p[:, 2], p[:, 1]] / 100

def get_co(me):
    co = np.empty(len(me.vertices) * 3); me.vertices.foreach_get('co', co); return co.reshape(-1, 3)
def set_co(me, co):
    me.vertices.foreach_set('co', co.astype(np.float32).ravel()); me.update()

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
bpy.ops.import_scene.gltf(filepath=GLB)
hi = [o for o in sc.objects if o.type == 'MESH'][0]
hi.name = 'hi'
# bake object transform into the mesh, then fit: T (glTF cm of the normalised model) -> W = T*S -> TPS
me = hi.data
M = np.array(hi.matrix_world)
co = get_co(me) @ M[:3, :3].T + M[:3, 3]
hi.matrix_world = Matrix.Identity(4)
skel = load_skeleton(MDH)
LW = landmarks_work(LMK)
posed = posed_skeleton(skel, LW)
src, dst = control_points(skel, posed, LW)
tps = TPS(src, dst)
W = tps(b2w(co) * S)
set_co(me, w2b(W))
with bpy.context.temp_override(object=hi, active_object=hi):
    try:
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    except Exception as e:
        print('no custom normals:', e)
for p in me.polygons: p.use_smooth = True
stamp('fit')

# low: weld the UV-split copy, then decimate it
weld = me.copy()
bm = bmesh.new(); bm.from_mesh(weld)
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
bm.to_mesh(weld); bm.free()
src_obj = bpy.data.objects.new('lo_src', weld); sc.collection.objects.link(src_obj)
dec = src_obj.modifiers.new('dec', 'DECIMATE')
dec.decimate_type = 'COLLAPSE'
dec.ratio = TRIS / len(weld.polygons)
dec.use_collapse_triangulate = True
dg = bpy.context.evaluated_depsgraph_get()
lo_me = bpy.data.meshes.new_from_object(src_obj.evaluated_get(dg))
bpy.data.objects.remove(src_obj); bpy.data.meshes.remove(weld)
lo = bpy.data.objects.new('lo', lo_me)
sc.collection.objects.link(lo)
bm = bmesh.new(); bm.from_mesh(lo_me)
bmesh.ops.triangulate(bm, faces=bm.faces[:])
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
bm.to_mesh(lo_me); bm.free()
for p in lo_me.polygons: p.use_smooth = True
src_uv = lo_me.uv_layers.active.name if lo_me.uv_layers else None
lo_me.uv_layers.new(name='bake')
lo_me.materials.clear()
stamp('decimate')
log['lo_tris'] = len(lo_me.polygons); log['lo_verts'] = len(lo_me.vertices); log['hi_tris'] = len(me.polygons)

# UV for the low mesh
for o in sc.objects: o.select_set(False)
bpy.context.view_layer.objects.active = lo; lo.select_set(True)
def edit(fn, uv_name):
    lo_me.uv_layers.active = lo_me.uv_layers[uv_name]
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    fn()
    bpy.ops.object.mode_set(mode='OBJECT')
if src_uv and os.environ.get('UV_MODE', 'islands') == 'islands':
    # seams where the decimated Tripo islands end, then a fresh low-stretch unwrap of those islands
    edit(lambda: bpy.ops.uv.seams_from_islands(mark_seams=True), src_uv)
    edit(lambda: (bpy.ops.uv.unwrap(method='MINIMUM_STRETCH', margin=0.002),
                  bpy.ops.uv.pack_islands(rotate=True, margin=0.004)), 'bake')
    log['seams'] = sum(e.use_seam for e in lo_me.edges)
else:
    edit(lambda: bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.003,
                                          area_weight=0.0, scale_to_bounds=True), 'bake')
if src_uv:
    lo_me.uv_layers.remove(lo_me.uv_layers[src_uv])
lo_me.uv_layers['bake'].active_render = True
log['uv_mode'] = os.environ.get('UV_MODE', 'islands')
stamp('uv')

# bake targets on the low mesh
mat = bpy.data.materials.new('OWN_HERO_BODY_TRIPO'); mat.use_nodes = True
lo_me.materials.append(mat)
nt = mat.node_tree
def image(name, noncolor, float_buffer=False):
    img = bpy.data.images.new(name, TEX, TEX, alpha=False, float_buffer=float_buffer)
    img.colorspace_settings.name = 'Non-Color' if noncolor else 'sRGB'
    node = nt.nodes.new('ShaderNodeTexImage'); node.image = img; node.name = name
    return node
nodes = {k: image(k, k != 'albedo', k == 'normal') for k in ('albedo', 'normal', 'roughness', 'metallic', 'ao')}

sc.render.engine = 'CYCLES'
sc.world = bpy.data.worlds.new('w')
sc.world.light_settings.distance = float(os.environ.get('AO_DIST', '0.08'))
sc.cycles.device = 'CPU'
sc.render.threads_mode = 'FIXED'; sc.render.threads = int(os.environ.get('BAKE_THREADS', '16'))
bk = sc.render.bake
bk.use_selected_to_active = True
bk.cage_extrusion = 0.01
bk.max_ray_distance = 0.04
bk.margin = 12
bk.margin_type = 'EXTEND'

hi_mat = hi.data.materials[0]
hnt = hi_mat.node_tree
bsdf = next(n for n in hnt.nodes if n.type == 'BSDF_PRINCIPLED')
out_node = next(n for n in hnt.nodes if n.type == 'OUTPUT_MATERIAL')
surface_link = out_node.inputs['Surface'].links[0].from_socket

def bake(kind, node, samples=1, **kw):
    sc.cycles.samples = samples
    for n in nt.nodes: n.select = False
    nt.nodes.active = node; node.select = True
    for o in sc.objects: o.select_set(False)
    hi.select_set(True); lo.select_set(True); bpy.context.view_layer.objects.active = lo
    bpy.ops.object.bake(type=kind, use_clear=True, **kw)
    stamp('bake_' + node.name)

def bake_input(socket_name, node):
    """Bake whatever drives a Principled input through an emission shader."""
    inp = bsdf.inputs[socket_name]
    em = hnt.nodes.new('ShaderNodeEmission')
    if inp.links:
        hnt.links.new(inp.links[0].from_socket, em.inputs['Color'])
    else:
        v = inp.default_value; em.inputs['Color'].default_value = (v, v, v, 1)
    hnt.links.new(em.outputs['Emission'], out_node.inputs['Surface'])
    bake('EMIT', node)
    hnt.links.new(surface_link, out_node.inputs['Surface'])
    hnt.nodes.remove(em)

bake('DIFFUSE', nodes['albedo'], pass_filter={'COLOR'})
bake('NORMAL', nodes['normal'], normal_space='TANGENT', normal_r='POS_X', normal_g='POS_Y', normal_b='POS_Z')
bake_input('Roughness', nodes['roughness'])
bake_input('Metallic', nodes['metallic'])
bake('AO', nodes['ao'], samples=32)
for k, n in nodes.items():
    img = n.image
    px = np.empty(TEX * TEX * 4, np.float32); img.pixels.foreach_get(px)
    np.save(f'{OUT}/{k}.npy', px.reshape(TEX, TEX, 4)[::-1].astype(np.float16))  # rows top-down
stamp('save_maps')

# armature from the posed Gothic skeleton, bone heat weights
names = skel[0]
tz = tz_of(LW)
J = {n: w2b(g2w(posed[i][:3, 3], tz)[None])[0] for i, n in enumerate(names)}
def along(a, b, length):
    d = J[b] - J[a]; return J[b] + d / np.linalg.norm(d) * length
segs = {
    'BIP01 PELVIS': (J['BIP01 PELVIS'], J['BIP01 PELVIS'] + np.array([0, 0, -0.09])),
    'BIP01 SPINE': (J['BIP01 SPINE'], J['BIP01 SPINE1']),
    'BIP01 SPINE1': (J['BIP01 SPINE1'], J['BIP01 SPINE2']),
    'BIP01 SPINE2': (J['BIP01 SPINE2'], J['BIP01 NECK']),
    'BIP01 NECK': (J['BIP01 NECK'], J['BIP01 HEAD']),
    'BIP01 HEAD': (J['BIP01 HEAD'], J['BIP01 HEAD'] + np.array([0, 0, 0.22])),
}
for s in 'LR':
    p = f'BIP01 {s} '
    segs[p + 'CLAVICLE'] = (J[p + 'CLAVICLE'], J[p + 'UPPERARM'])
    segs[p + 'UPPERARM'] = (J[p + 'UPPERARM'], J[p + 'FOREARM'])
    segs[p + 'FOREARM'] = (J[p + 'FOREARM'], J[p + 'HAND'])
    segs[p + 'HAND'] = (J[p + 'HAND'], J[p + 'FINGER0'])
    segs[p + 'FINGER0'] = (J[p + 'FINGER0'], along(p + 'HAND', p + 'FINGER0', 0.07))
    segs[p + 'THIGH'] = (J[p + 'THIGH'], J[p + 'CALF'])
    segs[p + 'CALF'] = (J[p + 'CALF'], J[p + 'FOOT'])
    segs[p + 'FOOT'] = (J[p + 'FOOT'], J[p + 'TOE0'])
    toe = J[p + 'TOE0'] - J[p + 'FOOT']; toe[2] = 0
    segs[p + 'TOE0'] = (J[p + 'TOE0'], J[p + 'TOE0'] + toe / np.linalg.norm(toe) * 0.06)
parent_of = {n: (names[skel[1][i]] if skel[1][i] >= 0 else None) for i, n in enumerate(names)}
arm_data = bpy.data.armatures.new('gothic'); arm = bpy.data.objects.new('gothic', arm_data)
sc.collection.objects.link(arm)
for o in sc.objects: o.select_set(False)
bpy.context.view_layer.objects.active = arm; arm.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
eb = {}
for n in names:
    if n not in segs: continue
    b = arm_data.edit_bones.new(n); b.head = Vector(segs[n][0]); b.tail = Vector(segs[n][1]); eb[n] = b
for n, b in eb.items():
    p = parent_of[n]
    while p is not None and p not in eb: p = parent_of[p]
    if p: b.parent = eb[p]
bpy.ops.object.mode_set(mode='OBJECT')
for o in sc.objects: o.select_set(False)
lo.select_set(True); arm.select_set(True); bpy.context.view_layer.objects.active = arm
bpy.ops.object.parent_set(type='ARMATURE_AUTO')
stamp('weights')

# export arrays
lo_me = lo.data
vg = [g.name for g in lo.vertex_groups]
Wt = np.zeros((len(lo_me.vertices), len(vg)), np.float32)
for v in lo_me.vertices:
    for g in v.groups: Wt[v.index, g.group] = g.weight
co = b2w(get_co(lo_me))
lo_me.calc_loop_triangles()
tri_v = np.empty(len(lo_me.loop_triangles) * 3, np.int32); lo_me.loop_triangles.foreach_get('vertices', tri_v)
tri_l = np.empty(len(lo_me.loop_triangles) * 3, np.int32); lo_me.loop_triangles.foreach_get('loops', tri_l)
uv = np.empty(len(lo_me.loops) * 2, np.float32); lo_me.uv_layers['bake'].data.foreach_get('uv', uv)
ln = np.empty(len(lo_me.loops) * 3, np.float32); lo_me.corner_normals.foreach_get('vector', ln)
ln = ln.reshape(-1, 3); ln = np.c_[ln[:, 0], ln[:, 2], -ln[:, 1]]
np.savez_compressed(f'{OUT}/lo.npz', co=co, tri_v=tri_v.reshape(-1, 3), tri_l=tri_l.reshape(-1, 3),
                    uv=uv.reshape(-1, 2), loop_normal=ln, weights=Wt, groups=np.array(vg))
log['unweighted'] = int((Wt.sum(1) < 1e-6).sum()); log['groups'] = vg
stamp('export')

# preview renders: rest and a test pose, albedo shown
for n in nt.nodes: n.select = False
nt.nodes.active = nodes['albedo']
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'TEXTURE'
sc.render.resolution_x, sc.render.resolution_y = 700, 1000
sc.render.film_transparent = True
hi.hide_render = True
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = 2.05; cam.data.sensor_fit = 'VERTICAL'
views = {'front': ((0, -5, 0.95), (math.pi / 2, 0, 0)), 'left': ((5, 0, 0.95), (math.pi / 2, 0, math.pi / 2)),
         'back': ((0, 5, 0.95), (math.pi / 2, 0, math.pi))}
def shoot(tag):
    for k, (loc, rot) in views.items():
        cam.location = loc; cam.rotation_euler = rot
        sc.render.filepath = f'{OUT}/prev-{tag}-{k}.png'; bpy.ops.render.render(write_still=True)
shoot('rest')
pb = arm.pose.bones
def rot(bone, axis, deg):
    b = pb[bone]; b.rotation_mode = 'XYZ'; setattr(b.rotation_euler, axis, math.radians(deg))
# bend in the bone's own axes (Blender bones point along +Y)
rot('BIP01 L UPPERARM', 'x', -70); rot('BIP01 L FOREARM', 'x', 80)
rot('BIP01 R THIGH', 'x', 50); rot('BIP01 R CALF', 'x', -70)
rot('BIP01 SPINE1', 'x', 15); rot('BIP01 HEAD', 'z', 30)
bpy.context.view_layer.update()
shoot('pose')
stamp('renders')
json.dump(log, open(f'{OUT}/log.json', 'w'), indent=1)
print('DONE', json.dumps(log))
