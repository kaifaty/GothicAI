# The atlas of hair strands for hair cards: eight clumps side by side, strands along v, the root at the top.
#   blender -b -P strand_atlas.py -- <out dir> [seed] [dark r,g,b] [mid r,g,b] [light r,g,b]
# Each clump is a set of strands, every strand a line from the root down: where it starts, how far it reaches, how it
# waves and which neighbours it sticks to. The strands are built as thin ribbons and rendered from above, so the
# nearer strand covers the farther one and the edge of a strand is a share of a pixel, not a step.
# Clumps (a tile is 256 x 2048 px; on a card 2 cm wide that is 0.08 mm a pixel):
#   0-2  wide, straight, dense    the lower layer that lies on the skull
#   3-5  locks of 5-7 clumps      the middle layer, the volume
#   6    narrow greasy locks      the top layer
#   7    single hairs             the edge of the hairstyle, hairs that got loose; at its bottom a swatch of leather
# Colours are sRGB 0..255: the shade between hairs, the hair, the lit hair; one strand in seven is lighter still.
# Writes hair_albedo.png (RGBA: colour, coverage) and hair_map.png (R shade: the root and the deep strands darker,
# G a random number of the strand, B the depth of the strand in the clump, A coverage).
import bpy, sys, os
import numpy as np

a = sys.argv[sys.argv.index('--')+1:]; OUT = a[0]; rng = np.random.default_rng(int(a[1]) if len(a) > 1 else 1)
col = lambda s, d: np.array([float(x) for x in s.split(',')]) if s else np.array(d, float)
DARK, MID, LIGHT = (col(a[i] if len(a) > i else None, d) for i, d in ((2, (55, 40, 28)), (3, (80, 59, 42)), (4, (120, 89, 69))))
lin = lambda c: np.where(c <= 0.04045, c/12.92, ((c+0.055)/1.055)**2.4)
T, H, K = 8, 8.0, 40                                       # tiles, height of a tile in tile widths, points of a strand
HAIR = 0.0075                                              # half width of a strand in tile widths: 0.3 mm on a 2 cm card, a few hairs stuck together -
                                                           # a single hair is under half a pixel of the game and an alpha test would cut it into dashes

def clump(kind):
    """Strands of one tile: x(t) across 0..1, t down 0..1, per strand the end, the depth 0..1 and the width."""
    if kind == 'wide':   n, locks, spread, wave, ends = 340, 16, 0.030, 0.006, (0.85, 1.0)
    elif kind == 'lock': n, locks, spread, wave, ends = 150, 6, 0.022, 0.012, (0.62, 1.0)
    elif kind == 'greasy': n, locks, spread, wave, ends = 80, 3, 0.012, 0.018, (0.55, 0.98)
    else:                n, locks, spread, wave, ends = 11, 11, 0.0, 0.035, (0.45, 0.95)
    t = np.linspace(0, 1, K); out = []
    cx = np.sort(rng.uniform(0.10, 0.90, locks)) if kind != 'wide' else np.linspace(0.03, 0.97, locks)
    drift = rng.normal(0, 0.05 if kind != 'wide' else 0.015, locks)      # a lock leans as it goes down
    phase = rng.uniform(0, 6.28, locks); freq = rng.uniform(1.5, 3.5, locks)
    for i in range(n):
        j = rng.integers(locks); end = rng.uniform(*ends)
        x0 = cx[j]+rng.normal(0, spread*2.2)                              # wide at the root, gathered at the tip
        tip = cx[j]+drift[j]+rng.normal(0, spread*0.5)
        x = x0+(tip-x0)*t**0.8+wave*np.sin(phase[j]+freq[j]*6.28*t*0.5)*t+rng.normal(0, 0.003)*np.sin(rng.uniform(0, 6.28)+9*t)
        out.append((np.clip(x, 0.015, 0.985), end, rng.uniform(), HAIR*rng.uniform(0.8, 1.25)))
    return out

KINDS = ['wide', 'wide', 'wide', 'lock', 'lock', 'lock', 'greasy', 'single']
verts, faces, calb, cmap = [], [], [], []
for ti, kind in enumerate(KINDS):
    for x, end, depth, hw in clump(kind):
        t = np.linspace(0, 1, K)*end; xs = np.interp(t, np.linspace(0, 1, K), x)
        w = hw*np.clip(1-((t/end)-0.75)/0.25, 0.25, 1)*np.clip(0.4+t/0.02, 0.4, 1)     # thin at the tip, at the very root too
        f_ = rng.uniform(-0.7, 1.0); c = MID+(LIGHT-MID)*f_ if f_ > 0 else MID+(MID-DARK)*f_
        if rng.uniform() < 1/7: c = LIGHT*np.array([1.18, 1.10, 0.92])                # a sun-bleached strand
        shade = (0.70+0.30*np.clip(t/0.35, 0, 1))*(0.72+0.28*depth)                    # the root and the deep strands are darker
        base = len(verts)
        for k in range(K):
            for sgn in (-1, 1):
                verts.append((ti+xs[k]+sgn*w[k], H*(1-t[k]), depth*0.5))
                calb.append(tuple(lin(np.clip(c*shade[k]/255, 0, 1)))+(1.0,)); cmap.append((shade[k], hash((ti, len(verts)//(2*K))) % 1000/1000.0, depth, 1.0))
        sid = rng.uniform()
        for k in range(2*K): cmap[base+k] = (cmap[base+k][0], sid, depth, 1.0)
        for k in range(K-1):
            faces.append((base+2*k, base+2*k+1, base+2*k+3, base+2*k+2))

# a swatch of leather for the cord of a ponytail, under the single hairs of the last tile
base = len(verts); verts += [(7.06, 0.03, 0), (7.94, 0.03, 0), (7.94, 0.30, 0), (7.06, 0.30, 0)]; faces.append((base, base+1, base+2, base+3))
calb += [tuple(lin(np.array([62, 40, 26])/255))+(1.0,)]*4; cmap += [(1.0, 0.0, 1.0, 1.0)]*4
bpy.ops.wm.read_factory_settings(use_empty=True)
me = bpy.data.meshes.new('strands'); me.from_pydata(verts, [], faces); me.update()
for name, data in (('alb', calb), ('map', cmap)):
    me.color_attributes.new(name, 'FLOAT_COLOR', 'POINT').data.foreach_set('color', np.array(data, np.float32).ravel())
ob = bpy.data.objects.new('strands', me); sc = bpy.context.scene
sc.collection.objects.link(ob)
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
cam.data.type = 'ORTHO'; cam.data.ortho_scale = T; cam.location = (T/2, H/2, 5); cam.rotation_euler = (0, 0, 0)
sc.render.resolution_x = sc.render.resolution_y = 2048; sc.render.film_transparent = True
sc.render.image_settings.file_format = 'PNG'; sc.render.image_settings.color_mode = 'RGBA'; sc.render.image_settings.color_depth = '8'
sc.render.engine = 'BLENDER_EEVEE'; sc.eevee.taa_render_samples = 64; sc.render.filter_size = 1.0
sc.world = bpy.data.worlds.new('w'); sc.world.use_nodes = True; sc.world.node_tree.nodes['Background'].inputs[1].default_value = 0
mat = bpy.data.materials.new('strand'); mat.use_nodes = True; nt = mat.node_tree; nt.nodes.clear()
attr = nt.nodes.new('ShaderNodeVertexColor'); em = nt.nodes.new('ShaderNodeEmission'); o = nt.nodes.new('ShaderNodeOutputMaterial')
nt.links.new(attr.outputs['Color'], em.inputs['Color']); nt.links.new(em.outputs[0], o.inputs['Surface']); me.materials.append(mat)
os.makedirs(OUT, exist_ok=True)
for layer, view, name in (('alb', 'Standard', 'hair_albedo.png'), ('map', 'Raw', 'hair_map.png')):
    attr.layer_name = layer; sc.view_settings.view_transform = view; sc.view_settings.look = 'None'
    sc.render.filepath = os.path.join(OUT, name); bpy.ops.render.render(write_still=True)
print('ATLAS strands %d, verts %d' % ((len(faces)-1)//(K-1), len(verts)))
