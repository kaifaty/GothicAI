"""Offline preview of the render rig: Gothic's own body and ours on the rig, side by side, on any Gothic clip.

    BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
      Blender -b <rig run>/body.blend -P rig_preview.py -- <out dir> <models dir of the base> <content dir> \
        <CLIP>[,<CLIP>...] [variants: engine:1:0,v2:0.038:0.5] [views: side,back,front]

Each variant is legs:lift:soft of rig_solve.py (for v2 the lift is its share over the lower ankle, else a scale of the
pelvis height). The clip plays in place (Gothic's root travel taken out). Frames go to
<out>/<clip>/<actor>-<view>/; the caller stacks them (ffmpeg hstack). Workbench, orthographic, the
camera follows the pelvis. Gothic's body is HUM_BODY_NAKED0 skinned by plain linear blending of its bind positions.
"""
import bpy, os, sys, math
from pathlib import Path
import numpy as np
from mathutils import Matrix, Vector

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent / 'anim'))
import gothic_anim as ga
import rig_solve as rs

argv = sys.argv[sys.argv.index('--') + 1:]
OUT, MODELS, CONTENT = Path(argv[0]), Path(argv[1]), Path(argv[2])
CLIPS = argv[3].split(',')
VARIANTS = [(v.split(':')[0], float(v.split(':')[1]), float(v.split(':')[2]))
            for v in (argv[4] if len(argv) > 4 else 'engine:1:0,v2:0.038:0.5').split(',')]
VIEWS = (argv[5] if len(argv) > 5 else 'side,back').split(',')
LOOPS = int(os.environ.get('LOOPS', 1))
RES = (int(os.environ.get('W', 420)), int(os.environ.get('H', 640)))
ZOOM = os.environ.get('ZOOM', 'whole')            # whole | legs | upper

rig = rs.Rig(MODELS, CONTENT)
arm = bpy.data.objects['Human.rig']; body = bpy.data.objects['Human']
bones = arm.data.bones
rest = {b.name: np.array(b.matrix_local) for b in bones}       # armature space (the armature sits at the origin)

# --- Gothic (cm, y up, z ahead) <-> Blender (m, z up): p_g = s * Q p_b + t, fitted on the rig's bind
Q = np.array([[-1.0, 0, 0], [0, 0, 1.0], [0, -1.0, 0]])
names = [n.name for n in rig.rk.nodes if n.name in rest]
pb_ = np.array([Q @ rest[n][:3, 3] for n in names]); pg_ = np.array([rig.Rb[rig.ri[n]][:3, 3] for n in names])
mb, mg = pb_.mean(0), pg_.mean(0)
s = float(((pb_ - mb) * (pg_ - mg)).sum() / ((pb_ - mb) ** 2).sum())
t = mg - s * mb
err = np.abs(s * pb_ + t - pg_).max()
print(f'PREVIEW map: {s:.2f} cm/m, bind misfit {err:.2f} cm')
to_b = lambda p: Q.T @ ((np.asarray(p) - t) / s)
def mat_b(G):
    M = np.eye(4); M[:3, :3] = Q.T @ G[:3, :3] @ Q; M[:3, 3] = to_b(G[:3, 3])
    return M

# --- scene
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'MATERIAL'
sc.display.shading.show_cavity = True; sc.display.shading.show_specular_highlight = False
sc.render.resolution_x, sc.render.resolution_y = RES
sc.render.image_settings.file_format = 'PNG'
skin = bpy.data.materials.new('skin_view'); skin.diffuse_color = (0.62, 0.47, 0.38, 1)
body.data.materials.clear(); body.data.materials.append(skin)
for o in bpy.data.objects:
    if o.type == 'MESH' and o is not body:
        o.hide_render = True
ground_z = to_b([0, 0, 0])[2]
gmat = bpy.data.materials.new('ground'); gmat.diffuse_color = (0.30, 0.30, 0.29, 1)
bpy.ops.mesh.primitive_plane_add(size=30, location=(0, 0, ground_z)); g = bpy.context.object
g.data.materials.append(gmat)
lmat = bpy.data.materials.new('line'); lmat.diffuse_color = (0.15, 0.15, 0.15, 1)
for i in range(-20, 21):                    # marks every 25 cm both ways: sliding and sinking feet show against them
    for rot in (0, 90):
        bpy.ops.mesh.primitive_cube_add(size=1, location=(0, i * 0.25, ground_z + 0.0005))
        c = bpy.context.object; c.scale = (8.0, 0.004, 0.001); c.data.materials.append(lmat)
        if rot:
            c.location = (i * 0.25, 0, ground_z + 0.0005); c.rotation_euler = (0, 0, math.radians(90))

# Gothic's body
gm = ga.read_skin_mesh(MODELS / 'HUM_BODY_NAKED0.MDM.json', rig.sk)
gme = bpy.data.meshes.new('gothic'); gme.from_pydata([tuple(to_b(p)) for p in gm.positions], [], gm.triangles.tolist())
gob = bpy.data.objects.new('gothic', gme); sc.collection.objects.link(gob)
gmat2 = bpy.data.materials.new('gothic_view'); gmat2.diffuse_color = (0.50, 0.52, 0.58, 1)
gme.materials.append(gmat2)
for p in gme.polygons:
    p.use_smooth = True
nv = len(gm.positions); nn = len(rig.sk.nodes)
Wt = np.zeros((nv, nn))
for v, ws in enumerate(gm.weights):
    for n, w in ws:
        Wt[v, n] += w
Pb = np.c_[gm.positions, np.ones(nv)]
inv_bind = np.linalg.inv(rig.Sb)

def pose_gothic(W):
    K = W @ inv_bind                                       # nn x 4 x 4
    P = np.einsum('vn,nij,vj->vi', Wt, K, Pb)[:, :3]
    gme.vertices.foreach_set('co', np.array([to_b(p) for p in P]).ravel())
    gme.update()

order = sorted(bones, key=lambda b: len(b.parent_recursive))
for pbn in arm.pose.bones:
    pbn.rotation_mode = 'QUATERNION'

def pose_ours(P):
    pose = {}
    for b in order:
        if b.name not in rig.ri:
            continue
        pose[b.name] = mat_b(P[rig.ri[b.name]])
    for b in order:
        if b.name not in pose:
            continue
        Mw = pose[b.name]
        if b.parent is None:
            basis = np.linalg.inv(rest[b.name]) @ Mw
        else:
            rel = np.linalg.inv(rest[b.parent.name]) @ rest[b.name]
            basis = np.linalg.inv(rel) @ np.linalg.inv(pose[b.parent.name]) @ Mw
        arm.pose.bones[b.name].matrix_basis = Matrix(basis.tolist())
    bpy.context.view_layer.update()

cam_d = bpy.data.cameras.new('view'); cam_d.type = 'ORTHO'
cam = bpy.data.objects.new('view', cam_d); sc.collection.objects.link(cam); sc.camera = cam
FR = {'whole': (0.95, 2.2), 'legs': (0.5, 1.15), 'upper': (1.35, 1.0)}
VIEW_DIR = {'side': (1, 0, 0), 'back': (0, 1, 0), 'front': (0, -1, 0), 'quarter': (0.7, -0.7, 0)}

def frame_cam(view, centre):
    h, size = FR[ZOOM]
    cam_d.ortho_scale = size
    c = Vector((centre[0], centre[1], ground_z + h))
    cam.location = c + Vector(VIEW_DIR[view]) * 5
    cam.rotation_euler = (c - cam.location).to_track_quat('-Z', 'Y').to_euler()

for clip in CLIPS:
    anim = ga.read_animation(MODELS / f'HUMANS-{clip}.MAN.json')
    root0 = None
    frames = []
    for f in range(anim.frame_count * LOOPS):
        W = ga.world_pose(rig.sk, anim, f % anim.frame_count)
        r = W[0][:3, 3].copy()
        root0 = r if root0 is None else root0
        W[:, 0, 3] -= r[0] - root0[0]; W[:, 2, 3] -= r[2] - root0[2]   # in place
        frames.append(W)
    centre = np.mean([to_b(W[rig.gi['BIP01 PELVIS']][:3, 3]) for W in frames], axis=0)   # the clip's own place
    actors = [('gothic', None)] + [(f'{l}-{lift:g}-{soft:g}', (l, lift, soft)) for l, lift, soft in VARIANTS]
    for name, var in actors:
        gob.hide_render = var is not None
        body.hide_render = var is None
        for view in VIEWS:
            frame_cam(view, centre)
            d = OUT / clip / f'{name}-{view}'; d.mkdir(parents=True, exist_ok=True)
            for i, W in enumerate(frames):
                if var is None:
                    pose_gothic(W)
                else:
                    if var[0] == 'v2':
                        rig.lift, rig.share = 1.0, var[1]
                    else:
                        rig.lift, rig.share = var[1], 0.0
                    rig.soft = var[2]
                    pose_ours(rig.solve(W, var[0]))
                sc.render.filepath = str(d / f'{i:04d}.png')
                bpy.ops.render.render(write_still=True)
    print('PREVIEW', clip, [a for a, _ in actors], VIEWS, len(frames), 'frames')
