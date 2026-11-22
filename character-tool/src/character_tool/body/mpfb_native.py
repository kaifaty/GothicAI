"""The MPFB man as its macros make him, without the fit to HUMANS.MDH: front and left orthographic views and his
joint heights, to set the human canon against Gothic's skeleton (research note anatomy.md).

    BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
      MUSCLE=0.6 WEIGHT=0.55 PROPORTIONS=1.0 Blender -b -P mpfb_native.py -- <out.png>

Writes <out>-front.png, -left.png, -back.png (ortho 2 m over 1000 px, camera centre at 0.95 m) and <out>.json: the joints
(cm above the soles, Blender frame: x left, z up) and the top of the head.
"""
import bpy, os, sys, json, math
import numpy as np
from mathutils import Vector
from bl_ext.user_default.mpfb.services import HumanService, TargetService

out = sys.argv[sys.argv.index('--') + 1]
MACRO = dict(gender=1.0, age=float(os.environ.get('AGE', 0.577)), muscle=float(os.environ.get('MUSCLE', 0.5)),
             weight=float(os.environ.get('WEIGHT', 0.5)), proportions=float(os.environ.get('PROPORTIONS', 1.0)),
             height=float(os.environ.get('HEIGHT', 0.5)))
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
macro = TargetService.get_default_macro_info_dict(); macro.update(MACRO)
macro['race'] = {'caucasian': 1.0, 'asian': 0.0, 'african': 0.0}
bm = HumanService.create_human(macro_detail_dict=macro)

kb = bm.data.shape_keys.key_blocks; nv = len(bm.data.vertices)   # the macros are shape keys: sum them
basis = np.empty(nv * 3); kb[0].data.foreach_get('co', basis); basis = basis.reshape(-1, 3)
co = basis.copy(); buf = np.empty(nv * 3)
for k in kb[1:]:
    if k.value != 0.0 and not k.mute:
        k.data.foreach_get('co', buf); co += k.value * (buf.reshape(-1, 3) - basis)
def group(name):
    gi = bm.vertex_groups[name].index
    return np.array([v.index for v in bm.data.vertices if any(g.group == gi and g.weight > 0.5 for g in v.groups)])
body = group('body')
ground = co[body, 2].min()
J = {k: ((co[group(f'joint-{k}')].mean(0) - [0, 0, ground]) * 100).round(1).tolist() for k in
     ('pelvis', 'l-upper-leg', 'l-knee', 'l-ankle', 'l-shoulder', 'l-elbow', 'l-hand', 'neck', 'head')}
top = float((co[body, 2].max() - ground) * 100)
json.dump({'macro': MACRO, 'joints_cm': J, 'top_cm': top}, open(out.replace('.png', '.json'), 'w'), indent=1)
bm.location.z -= ground
for o in bpy.data.objects:
    if o.type != 'MESH' or o is not bm:
        o.hide_render = True

sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'MATERIAL'; sc.display.shading.show_cavity = True
sc.render.resolution_x, sc.render.resolution_y = 700, 1000
cam = bpy.data.cameras.new('c'); cam.type = 'ORTHO'; cam.ortho_scale = 2.0
cob = bpy.data.objects.new('c', cam); sc.collection.objects.link(cob); sc.camera = cob
for name, ang in (('front', 0), ('left', 90), ('back', 180)):
    a = math.radians(ang)
    cob.location = Vector((math.sin(a) * 5, -math.cos(a) * 5, 0.95)); cob.rotation_euler = (math.radians(90), 0, a)
    sc.render.filepath = out.replace('.png', f'-{name}.png'); bpy.ops.render.render(write_still=True)
print('NATIVE', json.dumps({'top_cm': round(top, 1), 'joints': J}))
