# render_a2f.py plus eyeballs: the head's eyeLook* shapes move only the lids (eyes.md), so for the glance and the
# eye roll of the gesture probe the eye parts are also turned, rigidly about their centres, by angles from the same
# weights. The eyeballs are plain white there, so each gets a dark iris and pupil (caps carried by the eye) to show
# where it looks. Everything else is render_a2f.py as is.
#   blender -b --factory-startup -P render_cues.py -- <head.glb> <weights.csv> <out dir>/ [<size px, 480>]
# CUES_FRAMES=12,40 renders only those frames (stills for tuning), same file names f%04d.jpg.
import os, sys, math
from mathutils import Matrix, Vector

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'a2f', 'render_a2f.py')
TAIL = 'bpy.ops.render.render(animation=True)'
src = open(SRC).read().rstrip()
assert src.endswith(TAIL), 'render_a2f.py no longer ends with the render call'
exec(compile(src[:-len(TAIL)], SRC, 'exec'))

# degrees of eyeball turn at weight 1 (the lid shapes alone give ~4-14 degrees of lid, eyes.md)
SIDE_DEG, UP_DEG, DOWN_DEG = 30.0, 22.0, 22.0
J = {k: SKIN.index(k) for k in SKIN}
IRIS, PUPIL = 0.47, 0.20   # their radii as a share of the eyeball's

def sphere(o):
    """Least-squares sphere through the part's vertices: the mesh is not a whole ball, its box centre is off."""
    import numpy as np
    v = np.array([p.co[:] for p in o.data.vertices])
    A = np.c_[2*v, np.ones(len(v))]
    x = np.linalg.lstsq(A, (v*v).sum(1), rcond=None)[0]
    return Vector(x[:3]), float(np.sqrt(x[3]+x[:3]@x[:3]))

def cap(name, eye, c, R, rad, lift, color):
    """A spherical cap on the front of the eyeball (forward is -y), a hair above it, carried by the eye."""
    th = math.asin(rad)
    verts, faces, rings, seg = [], [], 6, 40
    for i in range(rings+1):
        a = th*i/rings
        for j in range(seg if i else 1):
            b = 2*math.pi*j/seg
            d = Vector((math.sin(a)*math.cos(b), -math.cos(a), math.sin(a)*math.sin(b)))
            verts.append(c+d*R*(1+lift))
    for j in range(seg):
        faces.append((0, 1+j, 1+(j+1) % seg))
    for i in range(1, rings):
        o0, o1 = 1+(i-1)*seg, 1+i*seg
        for j in range(seg):
            faces.append((o0+j, o1+j, o1+(j+1) % seg, o0+(j+1) % seg))
    m = bpy.data.meshes.new(name); m.from_pydata(verts, [], faces)
    ob = bpy.data.objects.new(name, m); sc.collection.objects.link(ob)
    ob.parent = eye; ob.color = color
    for poly in m.polygons:
        poly.use_smooth = True

eyes = []
for o in parts:
    if o.name in ('eye_L', 'eye_R'):
        c, R = sphere(o)
        cap(o.name+'_iris', o, c, R, IRIS, .004, (0.30, 0.24, 0.19, 1))
        cap(o.name+'_pupil', o, c, R, PUPIL, .008, (0.03, 0.03, 0.03, 1))
        eyes.append((o, c, o.name[-1]))
        print('eye', o.name, 'centre', tuple(round(x, 4) for x in c), 'radius', round(R, 4))

# GLB frame of this head: up is -x, forward is -y, the character's left is +z
def turn(scene, *_):
    w = W[min(max(scene.frame_current-1, 0), n-1)]
    for o, c, s in eyes:
        S = 'Left' if s == 'L' else 'Right'
        out, inn = w[J['eyeLookOut'+S]], w[J['eyeLookIn'+S]]
        left = (out - inn) if s == 'L' else (inn - out)          # toward the character's left
        up = w[J['eyeLookUp'+S]]*UP_DEG - w[J['eyeLookDown'+S]]*DOWN_DEG
        R = Matrix.Rotation(math.radians(-left*SIDE_DEG), 4, 'X') @ Matrix.Rotation(math.radians(-up), 4, 'Z')
        o.matrix_basis = Matrix.Translation(c) @ R @ Matrix.Translation(-c)
bpy.app.handlers.frame_change_pre.append(turn)

frames = os.environ.get('CUES_FRAMES')
if frames:
    for f in [int(x) for x in frames.split(',')]:
        sc.frame_set(f)
        sc.render.filepath = os.path.join(OUT, 'f%04d' % f)
        bpy.ops.render.render(write_still=True)
else:
    bpy.ops.render.render(animation=True)
