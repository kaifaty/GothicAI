"""MPFB hero body, relief: bake a tangent normal map and AO from a more defined copy of the body.

    BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
      Blender -b <run>/body.blend -P mpfb_bake.py -- <run> [size=2048]

The MPFB base mesh is low and smooth: its muscles are broad forms without the edges a strong man shows. The high
copy is subdivided and its own relief is amplified like an unsharp mask on geometry: d = co - smooth(co) along the
normal, times RELIEF. Fine skin (pores) is a bump on the high's material. Both bake onto the game mesh's UV:
<run>/normal.npy (tangent space, OpenGL +Y, rows top-down) and <run>/ao.npy, float16.
env: RELIEF (1.5), RELIEF_CAP (0.004 m), SMOOTH_ITERS (15), PORES (0.12), AO_DIST (0.06 m), BAKE_SAMPLES (16)
Hands, feet and the head get no boost: smoothing eats fingers, toes and ears whole.
The abdomen and the ribcage sides get muscles the base mesh lacks: ABS (0.0045 m), SERRATUS (0.0025 m), on the high after DENSE (3) subdivisions.
"""
import bpy, os, sys, bmesh
import numpy as np

argv = sys.argv[sys.argv.index('--') + 1:]
RUN = argv[0]; SIZE = int(argv[1]) if len(argv) > 1 else 2048
RELIEF = float(os.environ.get('RELIEF', 1.5)); ITERS = int(os.environ.get('SMOOTH_ITERS', 15))
PORES = float(os.environ.get('PORES', 0.12)); AO_DIST = float(os.environ.get('AO_DIST', 0.06))
CAP = float(os.environ.get('RELIEF_CAP', 0.004))   # m: the boost never moves the surface more than this
ABS = float(os.environ.get('ABS', 0.0045)); SERRATUS = float(os.environ.get('SERRATUS', 0.0025))   # m, sculpted heights
DENSE = int(os.environ.get('DENSE', 3))   # subdivision levels of the high: 3 gives about 2.5 mm on the belly

bm_obj = [o for o in bpy.data.objects if o.type == 'MESH' and 'body' in o.vertex_groups][0]
me = bm_obj.data; kb = me.shape_keys.key_blocks; nv = len(me.vertices)
basis = np.empty(nv * 3); kb[0].data.foreach_get('co', basis); basis = basis.reshape(-1, 3)
co = basis.copy(); buf = np.empty(nv * 3)
for k in kb[1:]:
    if k.value != 0.0 and not k.mute:
        k.data.foreach_get('co', buf); co += k.value * (buf.reshape(-1, 3) - basis)
gi = bm_obj.vertex_groups['body'].index
body = np.zeros(nv, bool)
for v in me.vertices:
    if any(g.group == gi and g.weight > 0.5 for g in v.groups): body[v.index] = True
faces = [p for p in me.polygons if body[list(p.vertices)].all()]
used = sorted({v for p in faces for v in p.vertices}); remap = {v: i for i, v in enumerate(used)}
uv_src = me.uv_layers['UVMap'].data

def make(name, verts):
    m = bpy.data.meshes.new(name)
    m.from_pydata(verts.tolist(), [], [[remap[v] for v in p.vertices] for p in faces])
    uvl = m.uv_layers.new(name='UVMap'); k = 0
    for p in faces:
        for li in p.loop_indices:
            uvl.data[k].uv = uv_src[li].uv; k += 1
    for poly in m.polygons: poly.use_smooth = True
    o = bpy.data.objects.new(name, m); bpy.context.scene.collection.objects.link(o); return o

for o in list(bpy.data.objects):
    o.hide_render = True
low = make('low', co[used])

# --- relief amplification on the high copy
P = co[used]
nbr = [[] for _ in used]
for p in faces:
    vs = [remap[v] for v in p.vertices]
    for a, b in zip(vs, vs[1:] + vs[:1]): nbr[a].append(b); nbr[b].append(a)
nbr = [np.array(sorted(set(n))) for n in nbr]
S = P.copy()
for _ in range(ITERS):
    S = 0.5 * S + 0.5 * np.array([S[n].mean(0) for n in nbr])
low.data.update()
N = np.array([v.normal[:] for v in low.data.vertices])
relief = ((P - S) * N).sum(1)
# no relief boost where the mesh is small detail (fingers, toes, ears, face): smoothing eats those whole
fade = np.zeros(nv)
for v in me.vertices:
    for g in v.groups:
        name = bm_obj.vertex_groups[g.group].name
        if name.startswith(('hand_', 'thumb_', 'index_', 'middle_', 'ring_', 'pinky_', 'foot_', 'ball_', 'head')):
            fade[v.index] = max(fade[v.index], g.weight)
keep = np.clip(1.0 - 1.5 * fade[used], 0.0, 1.0)
boost = np.clip(RELIEF * relief * keep, -CAP, CAP)
# --- muscles the base mesh does not have: rectus abdominis and serratus, sculpted on the dense mesh (metres)
def centroid(name):
    g = bm_obj.vertex_groups[name].index
    ids = [v.index for v in me.vertices if any(x.group == g and x.weight > 0.5 for x in v.groups)]
    return co[ids].mean(0)
from bl_ext.user_default.mpfb.services import TargetService
key = TargetService.load_target(bm_obj, TargetService.target_full_path('stomach-navel-in'), weight=0.0)
kd = np.empty(nv * 3); key.data.foreach_get('co', kd); move = np.linalg.norm(kd.reshape(-1, 3) - basis, axis=1)
bm_obj.shape_key_remove(key)
nav_ids = np.argsort(-move)[:12]
z_nav = float(co[nav_ids, 2].mean())                        # the navel: the vertices its own target moves most
z_nip = float((centroid('nippleTip') if 'nippleTip' in bm_obj.vertex_groups else centroid('nipple'))[2])
high = make('high', P + boost[:, None] * N)
vg = high.vertex_groups.new(name='keep')
for i, k in enumerate(keep): vg.add([i], float(k), 'REPLACE')
sub = high.modifiers.new('sub', 'SUBSURF'); sub.levels = sub.render_levels = DENSE
bpy.context.view_layer.objects.active = high; bpy.ops.object.modifier_apply(modifier='sub')
hm = high.data; hm.update()
D = np.empty(len(hm.vertices) * 3); hm.vertices.foreach_get('co', D); D = D.reshape(-1, 3)
ND = np.empty(len(hm.vertices) * 3); hm.vertices.foreach_get('normal', ND); ND = ND.reshape(-1, 3)
kg = high.vertex_groups['keep'].index
KD = np.zeros(len(hm.vertices))
for v in hm.vertices:
    for g in v.groups:
        if g.group == kg: KD[v.index] = g.weight
x, y, z = D[:, 0], D[:, 1], D[:, 2]
front = np.clip(-ND[:, 1] * 1.4 - 0.2, 0.0, 1.0)            # MPFB faces -Y
top, bottom = z_nip - 0.07, z_nav - 0.075
u = np.abs(x)
def smooth(e0, e1, t):
    q = np.clip((t - e0) / (e1 - e0), 0, 1); return q * q * (3 - 2 * q)
# a real belly, not a drawn grid: columns narrow towards the ribs, the tendinous lines curve up to the sides and
# differ left and right, the grooves are soft
width = 0.07 + 0.016 * np.clip((top - z) / (top - bottom), 0, 1)
col = np.sin(np.pi * np.clip(u / width, 0, 1)) ** 0.9
alba = np.exp(-(u / 0.006) ** 2)
lines = [(z_nav + 0.006, 0.0), (z_nav + (top - z_nav) * 0.37, 0.004), (z_nav + (top - z_nav) * 0.71, -0.003)]
seg = 1.0 - 0.6 * np.max([np.exp(-((z - (l + 0.012 * (u / 0.08) ** 2 + a * np.sign(x))) / 0.0085) ** 2)
                          for l, a in lines], axis=0)
band = smooth(bottom - 0.015, bottom + 0.025, z) * (1 - smooth(top - 0.02, top + 0.01, z))
abs_h = ABS * (col * seg - 0.3 * alba) * band * front * (u < 0.10)
ser = np.zeros_like(z)
for side in (-1, 1):
    for k in range(4):
        cx, cz = side * (0.118 + 0.008 * k), z_nip - 0.035 - 0.032 * k
        d_along = ((x - cx) * 0.7 * side + (z - cz) * 0.7)          # fingers slant down towards the front
        d_perp = (-(x - cx) * 0.7 * side + (z - cz) * 0.7)
        ser += np.exp(-(d_perp / 0.0075) ** 2 - (d_along / 0.022) ** 2)
side_facing = np.clip(np.abs(ND[:, 0]) * 1.6 - 0.2, 0, 1) * np.clip(-ND[:, 1] + 0.6, 0, 1)
ser_h = SERRATUS * ser * side_facing * (u > 0.09)
anat = (abs_h + ser_h) * KD
hm.vertices.foreach_set('co', (D + anat[:, None] * ND).ravel()); hm.update()
print('ANATOMY navel z', round(z_nav, 3), 'nipple z', round(z_nip, 3), 'dense verts', len(D),
      'abs mm max', round(float(abs_h.max() * 1000), 2), 'serratus mm max', round(float(ser_h.max() * 1000), 2))
print('RELIEF mm p50/p99', np.round(np.percentile(np.abs(boost) * 1000, [50, 99]), 2).tolist())

# --- materials: the high carries the pores as a bump, the low holds the bake target
mh = bpy.data.materials.new('high'); mh.use_nodes = True; nt = mh.node_tree
bsdf = nt.nodes['Principled BSDF']
noise = nt.nodes.new('ShaderNodeTexNoise'); noise.inputs['Scale'].default_value = 900.0; noise.inputs['Detail'].default_value = 3.0
vor = nt.nodes.new('ShaderNodeTexVoronoi'); vor.inputs['Scale'].default_value = 450.0
mix = nt.nodes.new('ShaderNodeMath'); mix.operation = 'MULTIPLY_ADD'; mix.inputs[1].default_value = 0.6
nt.links.new(vor.outputs['Distance'], mix.inputs[0]); nt.links.new(noise.outputs['Fac'], mix.inputs[2])
bump = nt.nodes.new('ShaderNodeBump'); bump.inputs['Strength'].default_value = PORES; bump.inputs['Distance'].default_value = 0.0004
nt.links.new(mix.outputs[0], bump.inputs['Height']); nt.links.new(bump.outputs['Normal'], bsdf.inputs['Normal'])
high.data.materials.append(mh)

def target(name, non_color=True):
    img = bpy.data.images.new(name, SIZE, SIZE, float_buffer=True, alpha=False)
    if non_color: img.colorspace_settings.name = 'Non-Color'
    ml = bpy.data.materials.new('low_' + name); ml.use_nodes = True
    node = ml.node_tree.nodes.new('ShaderNodeTexImage'); node.image = img
    ml.node_tree.nodes.active = node
    low.data.materials.clear(); low.data.materials.append(ml)
    return img

sc = bpy.context.scene
sc.render.engine = 'CYCLES'; sc.cycles.device = 'CPU'; sc.cycles.samples = int(os.environ.get('BAKE_SAMPLES', 16))
sc.render.threads_mode = 'FIXED'; sc.render.threads = int(os.environ.get('BAKE_THREADS', 6))
sc.world = sc.world or bpy.data.worlds.new('w'); sc.world.light_settings.distance = AO_DIST
sc.render.bake.margin = 16
for o in bpy.context.view_layer.objects: o.select_set(False)
high.hide_render = False; low.hide_render = False
high.select_set(True); low.select_set(True); bpy.context.view_layer.objects.active = low

def save(img, name, ch):
    a = np.array(img.pixels[:], dtype=np.float32).reshape(SIZE, SIZE, 4)[::-1, :, :ch]
    np.save(os.path.join(RUN, name), a.astype(np.float16)); return a

img = target('normal')
bpy.ops.object.bake(type='NORMAL', use_selected_to_active=True, cage_extrusion=0.012, max_ray_distance=0.03,
                    normal_space='TANGENT', margin=16)
n = save(img, 'normal.npy', 3)
img = target('ao')
bpy.ops.object.bake(type='AO', use_selected_to_active=True, cage_extrusion=0.012, max_ray_distance=0.03, margin=16)
ao = save(img, 'ao.npy', 1)
print('BAKED', SIZE, 'normal z p5', float(np.percentile(n[..., 2], 5)), 'ao mean', float(ao.mean()))
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(RUN, 'bake.blend'), copy=True)
