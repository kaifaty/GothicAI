# The registered head with a skin map, lit softly: front, three quarters, profile, from behind.
#   blender -b <round>/reg_sub.blend -P render_skin.py -- views.json skin.png <out dir> <prefix> [part.npz part.png]... [--eye eye.png] [--size 900]
# The cameras stand on the eye point of views.json, as likeness.py expects.
# The parts of hair_cut.py and beard_shell.py are put on the head; a map with alpha is cut at one half, as the game's
# alpha test does. --eye gives the eyeballs their map.
import bpy, sys, os, json, math
from mathutils import Vector
a = sys.argv[sys.argv.index('--')+1:]; V = json.load(open(a[0])); OUT, pre = a[2], a[3]
sc = bpy.context.scene; hm = bpy.data.objects['Human']; cam = sc.camera
mat = bpy.data.materials.new('skin_tex'); mat.use_nodes = True; nt = mat.node_tree
bsdf = nt.nodes['Principled BSDF']; tex = nt.nodes.new('ShaderNodeTexImage'); tex.image = bpy.data.images.load(a[1])
nt.links.new(tex.outputs['Color'], bsdf.inputs['Base Color']); bsdf.inputs['Roughness'].default_value = 0.8; bsdf.inputs['Specular IOR Level'].default_value = 0.15
skin_slots = [i for i, s in enumerate(hm.material_slots) if s.material and not any(k in s.material.name.lower() for k in ('eye', 'teeth', 'tongue', 'brow', 'lash'))]
for i in skin_slots: hm.material_slots[i].material = mat
EYE = None
if '--eye' in a:
    i = a.index('--eye'); EYE = a[i+1]; del a[i:i+2]
SIZE = 900
if '--size' in a:
    i = a.index('--size'); SIZE = int(a[i+1]); del a[i:i+2]
import numpy as np
for k in range(4, len(a)-1, 2):                                  # the parts: hair.npz hair.png, beard.npz beard.png, ...
    h = np.load(a[k]); name = os.path.splitext(os.path.basename(a[k]))[0]; me = bpy.data.meshes.new(name)
    me.from_pydata(h['v'].tolist(), [], h['f'].tolist()); me.update()
    uvl = me.uv_layers.new(name='UVMap'); uvl.data.foreach_set('uv', h['uv'].reshape(-1).astype('f'))
    for p in me.polygons: p.use_smooth = True
    if 'n' in h.files: me.normals_split_custom_set_from_vertices(h['n'].tolist())     # hair cards: normals of the hair's surface
    ho = bpy.data.objects.new(name, me); sc.collection.objects.link(ho)
    hmat = bpy.data.materials.new(name+'_tex'); hmat.use_nodes = True; hn = hmat.node_tree
    ht = hn.nodes.new('ShaderNodeTexImage'); ht.image = bpy.data.images.load(a[k+1])
    pb = hn.nodes['Principled BSDF']; hn.links.new(ht.outputs['Color'], pb.inputs['Base Color'])
    pb.inputs['Roughness'].default_value = 0.7; pb.inputs['Specular IOR Level'].default_value = 0.15
    if ht.image.channels == 4 and ht.image.alpha_mode != 'NONE':  # the game's alpha test: cut at one half
        cut = hn.nodes.new('ShaderNodeMath'); cut.operation = 'GREATER_THAN'; cut.inputs[1].default_value = 0.5
        hn.links.new(ht.outputs['Alpha'], cut.inputs[0]); hn.links.new(cut.outputs[0], pb.inputs['Alpha'])
    hmat.use_backface_culling = True                              # as the game draws it: a face turned inside out shows as a hole
    me.materials.append(hmat)
if EYE:                                                          # the eyeballs with the game's eye map
    for o in bpy.data.objects:
        if 'high-poly' in o.name:
            em = bpy.data.materials.new('eye_tex'); em.use_nodes = True; en = em.node_tree
            et = en.nodes.new('ShaderNodeTexImage'); et.image = bpy.data.images.load(EYE); pb = en.nodes['Principled BSDF']
            en.links.new(et.outputs['Color'], pb.inputs['Base Color']); en.links.new(et.outputs['Alpha'], pb.inputs['Alpha'])
            pb.inputs['Roughness'].default_value = 0.3
            for sl in o.material_slots: sl.material = em
for o in list(bpy.data.objects):
    if o.type == 'LIGHT': bpy.data.objects.remove(o)
for rx, rz, en in ((60, -30, 2.2), (60, 40, 1.4), (75, 180, 0.8)):
    ld = bpy.data.lights.new('l', 'SUN'); ld.energy = en; ld.angle = 0.5; lo = bpy.data.objects.new('l', ld); sc.collection.objects.link(lo)
    lo.rotation_euler = (math.radians(rx), 0, math.radians(rz))
sc.world.node_tree.nodes['Background'].inputs[1].default_value = 0.6
sc.view_settings.view_transform = 'Standard'
ex_, ey, ez = V['eye']                                         # the template's eye point: the registration moves the head's own eyes
sc.render.resolution_x = sc.render.resolution_y = SIZE; cam.data.type = 'ORTHO'; cam.data.ortho_scale = 0.36
for name, ang in (('front', 0), ('quarter', 35), ('profile', 90), ('back', 150)):
    r = math.radians(ang)
    cam.location = Vector((math.sin(r)*3, -math.cos(r)*3+ (ey+0.06 if ang == 90 else 0)*0, ez-0.03)); cam.rotation_euler = (math.radians(90), 0, r)
    if ang == 90: cam.location.y = ey+0.06
    sc.render.filepath = os.path.join(OUT, f'{pre}_{name}.png'); bpy.ops.render.render(write_still=True)
