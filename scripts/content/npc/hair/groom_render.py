# The head with a groom of strands, rendered in Cycles: the reference the game's hair is judged against.
#   blender -b <round>/reg_sub.blend -P groom_render.py -- views.json skin.png groom.npz <out dir> <prefix>
#       [--groom more.npz]... [--part part.npz part.png]... [--eye eye.png] [--ramp pigment.png] [--size 1200] [--spp 256]
#       [--views front,quarter,profile,back,hairline,knot] [--height skin_height.png] [--light portrait]
# skin.png is the colour map of the skin; for the likeness sheet, skin_material.py's skin_sheet.png: the game's map
# with Blender's neck at the colour of the game's body. The highlight is Blender's own (roughness 0.6), not the
# game's roughness map.
# The cameras stand on the eye point of views.json, as render_skin.py's do. Hair is Cycles' hair curves with the
# principled hair BSDF; a strand's colour is looked up in a ramp by its tint: --ramp is a strip of
# scripts/content/npc/concept_colours.py (hair_pigment.png), without it hero 3a's hand ramp. The cord of the tail is a
# band of leather. --groom adds more strands of the same hair: the beard, the moustache and the stubble of beard.py.
# --height: the relief of the skin that is finer than the mesh (skin_material.py's skin_height.png, 0.5 micrometres a
# step round 32768), as a bump: the sheet shows the wrinkles the game has in its normal map.
# --light portrait: the light of the concept sheets, for the likeness sheet: the key stands 45 degrees up and is wide,
# so the brows shade the eyes as they do on a concept; the fill and the sky are weaker; the eye is as wet as the
# game's (F0 0.02, a sharp highlight). Under the default light (the key 30 degrees up, a grey sky at 0.5) the whites
# glare and a grey veil of the sky lies on a dark iris: the second face's brown eyes came out grey.
import bpy, sys, os, json, math
import numpy as np
from mathutils import Vector

a = sys.argv[sys.argv.index('--')+1:]


def opt(name, default=None, n=1):
    out = []
    while name in a:
        i = a.index(name); out.append(a[i+1:i+1+n]); del a[i:i+1+n]
    return out if n > 1 else (out[-1][0] if out else default)


parts = opt('--part', n=2); MORE = []
while '--groom' in a:
    i = a.index('--groom'); MORE.append(a[i+1]); del a[i:i+2]
HEIGHT = opt('--height'); PORTRAIT = opt('--light') == 'portrait'; EYE = opt('--eye'); RAMP = opt('--ramp'); SIZE = int(opt('--size', 1200)); SPP = int(opt('--spp', 256))
VIEWS = opt('--views', 'front,quarter,profile,back,hairline,knot').split(',')
V = json.load(open(a[0])); G = np.load(a[2]); OUT, pre = a[3], a[4]
sc = bpy.context.scene; hm = bpy.data.objects['Human']; cam = sc.camera
lin = lambda c: tuple(((v/255+0.055)/1.055)**2.4 if v/255 > 0.04045 else v/255/12.92 for v in c)+(1.0,)

# ---- skin, eyes, the parts that are still shells
mat = bpy.data.materials.new('skin_tex'); mat.use_nodes = True; nt = mat.node_tree
bsdf = nt.nodes['Principled BSDF']; tex = nt.nodes.new('ShaderNodeTexImage'); tex.image = bpy.data.images.load(a[1])
nt.links.new(tex.outputs['Color'], bsdf.inputs['Base Color']); bsdf.inputs['Roughness'].default_value = 0.6; bsdf.inputs['Specular IOR Level'].default_value = 0.3
if HEIGHT:
    ht = nt.nodes.new('ShaderNodeTexImage'); ht.image = bpy.data.images.load(HEIGHT); ht.image.colorspace_settings.name = 'Non-Color'
    zero = nt.nodes.new('ShaderNodeMath'); zero.operation = 'SUBTRACT'; zero.inputs[1].default_value = 32768/65535
    bump = nt.nodes.new('ShaderNodeBump'); bump.inputs['Strength'].default_value = 1.0; bump.inputs['Distance'].default_value = 65535*5e-7
    nt.links.new(ht.outputs['Color'], zero.inputs[0]); nt.links.new(zero.outputs[0], bump.inputs['Height'])
    nt.links.new(bump.outputs['Normal'], bsdf.inputs['Normal'])
for i, s in enumerate(hm.material_slots):
    if s.material and not any(k in s.material.name.lower() for k in ('eye', 'teeth', 'tongue', 'brow', 'lash')): s.material = mat
for npz, png in parts:
    h = np.load(npz); name = os.path.splitext(os.path.basename(npz))[0]; me = bpy.data.meshes.new(name)
    me.from_pydata(h['v'].tolist(), [], h['f'].tolist()); me.update()
    me.uv_layers.new(name='UVMap').data.foreach_set('uv', h['uv'].reshape(-1).astype('f'))
    for p in me.polygons: p.use_smooth = True
    ho = bpy.data.objects.new(name, me); sc.collection.objects.link(ho)
    pm = bpy.data.materials.new(name+'_tex'); pm.use_nodes = True; pn = pm.node_tree
    pt = pn.nodes.new('ShaderNodeTexImage'); pt.image = bpy.data.images.load(png); pb = pn.nodes['Principled BSDF']
    pn.links.new(pt.outputs['Color'], pb.inputs['Base Color']); pb.inputs['Roughness'].default_value = 0.7
    cut = pn.nodes.new('ShaderNodeMath'); cut.operation = 'GREATER_THAN'; cut.inputs[1].default_value = 0.5
    pn.links.new(pt.outputs['Alpha'], cut.inputs[0]); pn.links.new(cut.outputs[0], pb.inputs['Alpha']); me.materials.append(pm)
if EYE:
    for o in bpy.data.objects:
        if 'high-poly' in o.name:
            em = bpy.data.materials.new('eye_tex'); em.use_nodes = True; en = em.node_tree
            et = en.nodes.new('ShaderNodeTexImage'); et.image = bpy.data.images.load(EYE); pb = en.nodes['Principled BSDF']
            en.links.new(et.outputs['Color'], pb.inputs['Base Color']); en.links.new(et.outputs['Alpha'], pb.inputs['Alpha'])
            pb.inputs['Roughness'].default_value = 0.12 if PORTRAIT else 0.3
            if PORTRAIT: pb.inputs['Specular IOR Level'].default_value = 0.25
            for sl in o.material_slots: sl.material = em

# ---- the strands
hmat = bpy.data.materials.new('hair'); hmat.use_nodes = True; hn = hmat.node_tree; hn.nodes.clear()
out = hn.nodes.new('ShaderNodeOutputMaterial'); hb = hn.nodes.new('ShaderNodeBsdfHairPrincipled'); hb.parametrization = 'COLOR'
at = hn.nodes.new('ShaderNodeAttribute'); at.attribute_name = 'tint'
mp = hn.nodes.new('ShaderNodeMapRange'); mp.inputs['From Min'].default_value = -1; mp.inputs['From Max'].default_value = 1
ramp = hn.nodes.new('ShaderNodeValToRGB'); el = ramp.color_ramp.elements
if RAMP:                                                                 # concept_colours.py's pigment strip, sRGB
    strip = bpy.data.images.load(RAMP); w = strip.size[0]; px = np.array(strip.pixels[:]).reshape(-1, w, 4)[0]
    for k in range(16):                                                  # a colour ramp holds at most 32 stops
        e = el[k] if k < 2 else el.new(k/15)
        e.position = k/15; e.color = lin(tuple(px[int(round(k/15*(w-1))), :3]*255))
else:
    el[0].position = 0.10; el[0].color = lin((20, 13, 8)); el[1].position = 0.95; el[1].color = lin((150, 92, 40))
    mid = el.new(0.55); mid.color = lin((56, 33, 17))                     # hero 3a's hair: shade, hair, the golden strands
hn.links.new(at.outputs['Fac'], mp.inputs['Value']); hn.links.new(mp.outputs['Result'], ramp.inputs['Fac'])
hn.links.new(ramp.outputs['Color'], hb.inputs['Color']); hn.links.new(hb.outputs['BSDF'], out.inputs['Surface'])
for k, v in (('Roughness', 0.33), ('Radial Roughness', 0.45), ('Random Roughness', 0.15), ('Random Color', 0.06), ('IOR', 1.55)):
    if k in hb.inputs: hb.inputs[k].default_value = v
n = 0
for gi, S in enumerate([G]+[np.load(m) for m in MORE]):
    pts = S['pts'].astype('f'); ns, K = pts.shape[:2]; n += ns
    cu = bpy.data.hair_curves.new('groom%d' % gi); cu.add_curves([K]*ns)
    cu.attributes['position'].data.foreach_set('vector', pts.reshape(-1))
    taper = np.clip((1-np.linspace(0, 1, K))/0.3, 0.25, 1.0)             # thinner over the last third
    rad = cu.attributes.get('radius') or cu.attributes.new('radius', 'FLOAT', 'POINT')
    rad.data.foreach_set('value', (S['rad'][:, None]*taper[None]).reshape(-1).astype('f'))
    cu.attributes.new('tint', 'FLOAT', 'CURVE').data.foreach_set('value', S['tint'].astype('f'))
    sc.collection.objects.link(bpy.data.objects.new(cu.name, cu)); cu.materials.append(hmat)

# ---- the cord: a band of leather round the tail's root
cd = G['cord']; cc, cax, cr, ca, cb = Vector(cd[0:3]), Vector(cd[3:6]).normalized(), float(cd[6])*0.70, float(cd[7]), float(cd[8])
bm = bpy.data.materials.new('cord'); bm.use_nodes = True; pb = bm.node_tree.nodes['Principled BSDF']
pb.inputs['Base Color'].default_value = lin((70, 42, 24)); pb.inputs['Roughness'].default_value = 0.6
for i in range(5):                                                       # five turns of a leather string
    bpy.ops.mesh.primitive_torus_add(major_radius=cr+0.0002*(i % 2), minor_radius=0.0011, major_segments=48, minor_segments=8,
                                     location=cc+cax*(ca+(cb-ca)*i/4))
    band = bpy.context.object; band.rotation_mode = 'QUATERNION'
    band.rotation_quaternion = Vector((0, 0, 1)).rotation_difference(cax+Vector((0.04*(i % 2), 0.03, 0)))
    bpy.ops.object.shade_smooth(); band.data.materials.append(bm)

# ---- light and render
for o in list(bpy.data.objects):
    if o.type == 'LIGHT': bpy.data.objects.remove(o)
LIGHTS = ((45, -20, 2.8, 0.6), (60, 50, 1.0, 0.4), (70, 180, 1.4, 0.25)) if PORTRAIT else ((60, -30, 2.6, 0.25), (60, 40, 1.2, 0.25), (70, 180, 1.4, 0.25))
for rx, rz, en, wide in LIGHTS:
    ld = bpy.data.lights.new('l', 'SUN'); ld.energy = en; ld.angle = wide; lo = bpy.data.objects.new('l', ld); sc.collection.objects.link(lo)
    lo.rotation_euler = (math.radians(rx), 0, math.radians(rz))
sc.world.node_tree.nodes['Background'].inputs[1].default_value = 0.4 if PORTRAIT else 0.5
sc.render.engine = 'CYCLES'; sc.cycles.samples = SPP; sc.cycles.use_denoising = False; sc.render.film_transparent = True
sc.cycles.max_bounces = 16; sc.cycles.transmission_bounces = 16; sc.cycles.transparent_max_bounces = 32
try:
    pr = bpy.context.preferences.addons['cycles'].preferences; pr.compute_device_type = 'METAL'; pr.get_devices()
    for d in pr.devices: d.use = d.type == 'METAL'
    sc.cycles.device = 'GPU'
except Exception as e: print('CPU render:', e)
sc.cycles_curves.shape = 'RIBBONS'; sc.view_settings.view_transform = 'Standard'
ex_, ey, ez = V['eye']; sc.render.resolution_x = sc.render.resolution_y = SIZE; cam.data.type = 'ORTHO'
SHOTS = {'front': (0, 0.36, 0, -0.03), 'quarter': (35, 0.36, 0, -0.03), 'profile': (90, 0.36, 0.06, -0.03), 'back': (150, 0.36, 0, -0.03),
         'behind': (180, 0.36, 0, -0.03), 'hairline': (20, 0.17, 0, 0.065), 'knot': (165, 0.20, 0, -0.045), 'top': (0, 0.30, 0, 0),
         'beard': (0, 0.17, 0, -0.085, -0.02), 'beard_q': (40, 0.17, 0, -0.085, -0.02), 'jaw': (90, 0.17, 0, -0.085, 0.02)}   # the last: aimed at a point this far behind the eyes
for name in VIEWS:
    ang, scale, shift, dz = SHOTS[name][:4]; r = math.radians(ang); cam.data.ortho_scale = scale
    if name == 'top':
        cam.location = Vector((0, ey+0.10, ez+3)); cam.rotation_euler = (0, 0, 0)
    else:
        cam.location = Vector((math.sin(r)*3, -math.cos(r)*3, ez+dz)); cam.rotation_euler = (math.radians(90), 0, r)
        if ang == 90: cam.location.y = ey+shift
        if len(SHOTS[name]) > 4: cam.location.y = -math.cos(r)*3+ey+SHOTS[name][4]
    sc.render.filepath = os.path.join(OUT, f'{pre}_{name}.png'); bpy.ops.render.render(write_still=True)
print('GROOM RENDER', n, 'strands;', ', '.join(VIEWS))
