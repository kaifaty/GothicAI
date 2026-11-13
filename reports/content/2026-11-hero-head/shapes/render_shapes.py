# Stills of chosen shape mixes on a head GLB, front and three-quarter, for comparing shape sets.
#   blender -b --factory-startup -P render_shapes.py -- <head.glb> <out dir> [--frame face|mouth|eyes] "jawOpen=1" \
#       "mouthSmileLeft=1,mouthSmileRight=1" "gaze=up:25,eyeLookUpLeft=1,eyeLookUpRight=1" ...
# Writes <out dir>/<i>_front.jpg and <i>_quarter.jpg per mix (Workbench, the light of render_a2f.py). --frame mouth or
# eyes frames the lips or the eyes close up; --hide brows,lashes leaves out those parts (Workbench draws their alpha cards
# as solid quads). gaze=<up|down|in|out>:<degrees> turns both eyeballs about their centres.
import bpy, os, sys, math
from mathutils import Vector

A = sys.argv[sys.argv.index('--')+1:]
FRAME, HIDE = 'face', []
if '--frame' in A:
    i = A.index('--frame'); FRAME = A[i+1]; del A[i:i+2]
if '--hide' in A:
    i = A.index('--hide'); HIDE = A[i+1].split(','); del A[i:i+2]
GLB, OUT, MIXES = A[0], A[1], A[2:]
os.makedirs(OUT, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
bpy.ops.import_scene.gltf(filepath=GLB)
parts = [o for o in sc.objects if o.type == 'MESH']
for o in [o for o in parts if o.name in HIDE]:        # alpha cards (brows, lashes) render as solid quads here
    bpy.data.objects.remove(o)
parts = [o for o in sc.objects if o.type == 'MESH']
pivot = bpy.data.objects.new('pivot', None); sc.collection.objects.link(pivot)
for o in parts:
    o.parent = pivot
pivot.rotation_euler = (0, math.radians(90), 0)
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'OBJECT'
sc.display.shading.show_cavity = True
sc.render.resolution_x = sc.render.resolution_y = 420
sc.world = bpy.data.worlds.new('w'); sc.world.color = (0.22, 0.22, 0.22)
for o in parts:
    o.color = (0.85, 0.72, 0.62, 1) if o.name == 'face' else (0.95, 0.95, 0.95, 1)
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = 0.17
bpy.context.view_layer.update()
face = next(o for o in parts if o.name == 'face')
pts = [face.matrix_world @ Vector(c) for c in face.bound_box]
ctr = sum(pts, Vector())/len(pts)
def movers(o, name, k):
    kb = o.data.shape_keys.key_blocks
    d = sorted(range(len(o.data.vertices)), key=lambda i: -(kb[name].data[i].co-kb['Basis'].data[i].co).length)[:k]
    return sum((o.matrix_world @ o.data.vertices[i].co for i in d), Vector())/k
eyes = {o.name: o for o in parts if o.name in ('eye_L', 'eye_R')}
centre = {n: sum((o.matrix_world @ v.co for v in o.data.vertices), Vector())/len(o.data.vertices) for n, o in eyes.items()}
if FRAME == 'mouth':
    # the lips by a viseme: every shape set here keeps MPFB's visemes, so the frame is the same on each head
    ctr = movers(face, 'viseme_O' if 'viseme_O' in face.data.shape_keys.key_blocks else 'mouthPucker', 12)
    cam.data.ortho_scale = 0.075
elif FRAME == 'eyes':
    ctr = (centre['eye_L']+centre['eye_R'])/2; cam.data.ortho_scale = 0.11
# the eyes turn about the centres of the spheres through them (as the engine's FaceRig does): least squares
def sphere(o):
    import numpy as np
    P = np.array([list(o.matrix_world @ v.co) for v in o.data.vertices])
    x = np.linalg.lstsq(np.c_[2*P, np.ones(len(P))], (P**2).sum(1), rcond=None)[0]
    return Vector(x[:3])
pivots = {}
for n, o in eyes.items():
    c = sphere(o); e = bpy.data.objects.new('pivot_'+n, None); sc.collection.objects.link(e)
    e.location = c; bpy.context.view_layer.update()
    mw = o.matrix_world.copy(); o.parent = e; o.matrix_world = mw; pivots[n] = e
up_w = (centre['eye_L']+centre['eye_R'])/2-movers(face, 'viseme_O' if 'viseme_O' in face.data.shape_keys.key_blocks else 'mouthPucker', 12); up_w.normalize()
left_w = centre['eye_L']-centre['eye_R']; left_w.normalize()
from mathutils import Matrix
def gaze(where, deg):
    for n, e in pivots.items():
        out = left_w if n == 'eye_L' else -left_w
        to = {'up': up_w, 'down': -up_w, 'out': out, 'in': -out}[where]
        fwd = up_w.cross(left_w); fwd.normalize()
        ax = fwd.cross(to); ax.normalize()
        e.matrix_world = Matrix.Translation(e.location) @ Matrix.Rotation(math.radians(deg), 4, ax)
sc.render.image_settings.file_format = 'JPEG'
for i, mix in enumerate(MIXES):
    for o in parts:
        if o.data.shape_keys:
            for k in o.data.shape_keys.key_blocks[1:]:
                k.value = 0.0
    for e in pivots.values():
        e.matrix_world = Matrix.Translation(e.location)
    for item in filter(None, mix.split(',')):
        n, v = item.split('=')
        if n == 'gaze':
            gaze(v.split(':')[0], float(v.split(':')[1])); continue
        for o in parts:
            if o.data.shape_keys and n in o.data.shape_keys.key_blocks:
                o.data.shape_keys.key_blocks[n].value = float(v)
    for view, a in (('front', 0), ('quarter', 35)):
        r = math.radians(a)
        cam.location = (ctr.x+math.sin(r)*2, ctr.y-math.cos(r)*2, ctr.z-(0.035 if FRAME == 'face' else 0))
        cam.rotation_euler = (math.radians(90), 0, r)
        sc.render.filepath = os.path.join(OUT, '%02d_%s.jpg' % (i, view))
        bpy.ops.render.render(write_still=True)
print('rendered', len(MIXES), 'mixes')
