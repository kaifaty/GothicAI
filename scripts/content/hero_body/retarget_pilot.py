"""Render-skeleton pilot: Gothic clips carried onto the MPFB man on his own rig (game_engine, 53 bones, UE names).

    BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender MUSCLE=0.75 WEIGHT=0.6 \
      Blender -b -P retarget_pilot.py -- <out dir> <HUMANS.MDH.json> <HUMANS-<CLIP>.MAN.json> [...]

The man keeps human proportions; only motion comes from Gothic (plan: reports/engine/2026-11-modern-skeleton):
- each mapped bone takes the Gothic node's turn from its bind pose, as a world-space delta: R = dR * R_rest;
- bone lengths are his own; the pelvis moves with Gothic's pelvis, displacement scaled by the hip heights;
- legs are solved by two-bone IK onto Gothic's ankle (5 cm under and 9 cm ahead of its foot node), the knee bending
  towards Gothic's knee, so the feet keep the clip's contacts while the knee sits where a human's does;
- Gothic has one FINGER0 per hand: its bend is spread over the three phalanges; the thumb follows the hand.
Writes <out>/<clip>-side.mp4 and <clip>-front.mp4 (Workbench) and pilot.json.
"""
import bpy, os, sys, json, math, subprocess
from pathlib import Path
import numpy as np
from mathutils import Matrix
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'anim'))
import gothic_anim as ga
from bl_ext.user_default.mpfb.services import HumanService, TargetService

argv = sys.argv[sys.argv.index('--') + 1:]
OUT, MDH, CLIPS = Path(argv[0]), Path(argv[1]), [Path(p) for p in argv[2:]]
OUT.mkdir(parents=True, exist_ok=True)
LOOPS = int(os.environ.get('LOOPS', 2))

# --- the man: macros only (anatomy.md), the groin under the braies
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
macro = TargetService.get_default_macro_info_dict()
macro.update(gender=1.0, age=0.577, muscle=float(os.environ.get('MUSCLE', 0.75)), weight=float(os.environ.get('WEIGHT', 0.6)),
             proportions=1.0, height=float(os.environ.get('HEIGHT', 0.5)))
macro['race'] = {'caucasian': 1.0, 'asian': 0.0, 'african': 0.0}
bm = HumanService.create_human(macro_detail_dict=macro)
TargetService.load_target(bm, TargetService.target_full_path('bulge-incr'), weight=1.0)
rig = HumanService.add_builtin_rig(bm, 'game_engine')
for o in bpy.data.objects:                       # helpers and joint cubes stay out of the frame
    if o.type == 'MESH' and o is not bm:
        o.hide_render = True

bones = rig.data.bones
rest = {b.name: np.array(b.matrix_local) for b in bones}
parent = {b.name: (b.parent.name if b.parent else None) for b in bones}
depth = lambda n: 0 if parent[n] is None else 1 + depth(parent[n])
order = sorted(rest, key=depth)
for pb in rig.pose.bones:
    pb.rotation_mode = 'QUATERNION'

# --- Gothic
sk = ga.read_skeleton(MDH)
gi = {n.name: i for i, n in enumerate(sk.nodes)}
bind = ga.world_pose(sk, None, 0)
C = np.array([[-1.0, 0, 0], [0, 0, -1.0], [0, 1.0, 0]])     # Gothic (x right, y up, z ahead, cm) -> Blender
k = rest['thigh_l'][2, 3] / bind[gi['BIP01 L THIGH']][1, 3]  # metres per Gothic cm: the hips at one height
pel_g = bind[gi['BIP01 PELVIS']][:3, 3]; pel_b = rest['pelvis'][:3, 3]
P = lambda p: C @ (p[:3] - pel_g) * k + pel_b
Rb = lambda R: C @ R @ C.T

MAP = {'pelvis': 'PELVIS', 'spine_01': 'SPINE', 'spine_02': 'SPINE1', 'spine_03': 'SPINE2', 'neck_01': 'NECK',
       'head': 'HEAD'}
for s, S in (('l', 'L'), ('r', 'R')):
    MAP.update({f'clavicle_{s}': f'{S} CLAVICLE', f'upperarm_{s}': f'{S} UPPERARM', f'lowerarm_{s}': f'{S} FOREARM',
                f'hand_{s}': f'{S} HAND', f'thigh_{s}': f'{S} THIGH', f'calf_{s}': f'{S} CALF', f'foot_{s}': f'{S} FOOT',
                f'ball_{s}': f'{S} TOE0'})
MAP = {b: gi[f'BIP01 {n}'] if n != 'PELVIS' else gi['BIP01 PELVIS'] for b, n in MAP.items()}
FINGERS = ('index', 'middle', 'ring', 'pinky')
CURL = (1.0, 0.9, 0.7)       # share of FINGER0's bend at each phalanx: a fist bends all three knuckles

# Gothic's ankle in its foot node's frame: the node sits 5 cm above and 9 cm behind a real ankle (bind pose)
ankle_o = {}
for S in 'LR':
    F = bind[gi[f'BIP01 {S} FOOT']]
    ankle_o[S] = np.linalg.inv(F) @ np.r_[F[:3, 3] + [0, -5.0, 9.0], 1.0]

def ankle_g(W, S):
    return (W[gi[f'BIP01 {S} FOOT']] @ ankle_o[S])[:3]

def rot_between(a, b):
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(a @ b)
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx / (1 + c)

def axis_angle(R):
    ang = math.acos(max(-1.0, min(1.0, (np.trace(R) - 1) / 2)))
    if ang < 1e-6:
        return np.array([1.0, 0, 0]), 0.0
    ax = np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]]) / (2 * math.sin(ang))
    return ax / np.linalg.norm(ax), ang

def rot_axis(ax, ang):
    vx = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
    return np.eye(3) + math.sin(ang) * vx + (1 - math.cos(ang)) * vx @ vx

# Retarget pose: MPFB stands in an A-pose, Gothic's bind has the arms down and its own leg and foot lines. Each mapped
# limb bone is first turned onto the direction of its Gothic counterpart in the bind pose (A), then takes the clip's
# turn. The spine, neck and head are not aligned: both stand upright in the bind pose, while Gothic's nodes run along
# the back with a bend and its head-to-helmet line leans 14 degrees forward (pilot 3: the head looked down).
node_p = lambda W, n: W[gi[f'BIP01 {n}']][:3, 3]
DIRS = {}
for s, S in (('l', 'L'), ('r', 'R')):
    DIRS.update({f'clavicle_{s}': (f'{S} CLAVICLE', f'{S} UPPERARM'), f'upperarm_{s}': (f'{S} UPPERARM', f'{S} FOREARM'),
                 f'lowerarm_{s}': (f'{S} FOREARM', f'{S} HAND'), f'hand_{s}': (f'{S} HAND', f'{S} FINGER0'),
                 f'thigh_{s}': (f'{S} THIGH', f'{S} CALF'), f'calf_{s}': (f'{S} CALF', ('ankle', S)),
                 f'foot_{s}': (('ankle', S), f'{S} TOE0')})
def gpoint(W, n):
    return ankle_g(W, n[1]) if isinstance(n, tuple) else node_p(W, n)
ALIGN = {b: rot_between(rest[b][:3, 1], C @ (gpoint(bind, c) - gpoint(bind, a))) for b, (a, c) in DIRS.items()}

def fk(pose, b):
    p = parent[b]
    return rest[b].copy() if p is None else pose[p] @ np.linalg.inv(rest[p]) @ rest[b]

def solve(W):
    """Armature-space pose of every bone for Gothic world transforms W (cm, Gothic frame)."""
    pose = {}
    for b in order:
        M = fk(pose, b)
        if b in MAP:
            s = MAP[b]
            M[:3, :3] = Rb(W[s][:3, :3] @ bind[s][:3, :3].T) @ ALIGN.get(b, np.eye(3)) @ rest[b][:3, :3]
        if b == 'pelvis':
            M[:3, 3] = P(W[MAP['pelvis']][:3, 3])
        pose[b] = M
    # fingers: Gothic FINGER0's bend relative to the hand, spread over the phalanges
    for s, S in (('l', 'L'), ('r', 'R')):
        h, f = gi[f'BIP01 {S} HAND'], gi[f'BIP01 {S} FINGER0']
        local_t = W[h][:3, :3].T @ W[f][:3, :3]; local_b = bind[h][:3, :3].T @ bind[f][:3, :3]
        bend_world = Rb(W[h][:3, :3] @ (local_t @ local_b.T) @ W[h][:3, :3].T)
        ax, ang = axis_angle(bend_world)
        for fn in FINGERS:
            for j, share in enumerate(CURL, 1):
                b = f'{fn}_0{j}_{s}'
                if b not in pose:
                    continue
                M = fk(pose, b)
                M[:3, :3] = rot_axis(ax, ang * share) @ M[:3, :3]
                pose[b] = M
                for child in order:                  # keep the rest of the chain on the turned phalanx
                    if parent[child] == b:
                        pose[child] = fk(pose, child)
    # legs: two-bone IK onto Gothic's ankle, the knee towards Gothic's knee
    for s, S in (('l', 'L'), ('r', 'R')):
        th, ca, fo, ba = f'thigh_{s}', f'calf_{s}', f'foot_{s}', f'ball_{s}'
        H = pose[th][:3, 3]
        L1 = np.linalg.norm(rest[ca][:3, 3] - rest[th][:3, 3]); L2 = np.linalg.norm(rest[fo][:3, 3] - rest[ca][:3, 3])
        T = P(ankle_g(W, S))
        pole = P(W[gi[f'BIP01 {S} CALF']][:3, 3])
        d = np.clip(np.linalg.norm(T - H), abs(L1 - L2) + 1e-4, L1 + L2 - 1e-4)
        u = (T - H) / np.linalg.norm(T - H); T = H + u * d
        a = (L1 * L1 - L2 * L2 + d * d) / (2 * d); hgt = math.sqrt(max(L1 * L1 - a * a, 0.0))
        v = (pole - H) - ((pole - H) @ u) * u
        v = v / np.linalg.norm(v) if np.linalg.norm(v) > 1e-6 else (pose[ca][:3, 3] - H - ((pose[ca][:3, 3] - H) @ u) * u)
        v = v / max(np.linalg.norm(v), 1e-9)
        K = H + a * u + hgt * v
        pose[th][:3, :3] = rot_between(pose[th][:3, :3] @ [0, 1.0, 0], K - H) @ pose[th][:3, :3]
        pose[ca][:3, :3] = rot_between(pose[ca][:3, :3] @ [0, 1.0, 0], T - K) @ pose[ca][:3, :3]
        pose[ca][:3, 3] = K; pose[fo][:3, 3] = T
        pose[ba][:3, 3] = (pose[fo] @ np.linalg.inv(rest[fo]) @ rest[ba])[:3, 3]
    return pose

def key(pose, frame):
    for b in order:
        p = parent[b]
        base = rest[b] if p is None else pose[p] @ np.linalg.inv(rest[p]) @ rest[b]
        pb = rig.pose.bones[b]
        pb.matrix_basis = Matrix((np.linalg.inv(base) @ pose[b]).tolist())
        pb.keyframe_insert('rotation_quaternion', frame=frame)
        pb.keyframe_insert('location', frame=frame)

# --- scene
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'MATERIAL'; sc.display.shading.show_cavity = True
sc.render.resolution_x, sc.render.resolution_y = 640, 800
sc.render.fps = 25
gmat = bpy.data.materials.new('ground'); gmat.diffuse_color = (0.32, 0.32, 0.30, 1)
lmat = bpy.data.materials.new('line'); lmat.diffuse_color = (0.12, 0.12, 0.12, 1)
bpy.ops.mesh.primitive_plane_add(size=40, location=(0, -8, 0)); bpy.context.object.data.materials.append(gmat)
for i in range(-20, 61):                          # a mark every 25 cm along the walk: sliding feet show against it
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, -i * 0.25, 0.001))
    c = bpy.context.object; c.scale = (3.0, 0.006, 0.002); c.data.materials.append(lmat)
# Gothic's own skeleton as a red stick figure through a see-through body (STICKS=1): the motion to match
STICKS = os.environ.get('STICKS', '1') == '1'
sticks = []
if STICKS:
    smat = bpy.data.materials.new('stick'); smat.diffuse_color = (0.9, 0.15, 0.1, 1)
    for i, n in enumerate(sk.nodes):
        if n.parent >= 0 and not n.name.startswith('ZS_') and not sk.nodes[n.parent].name == 'BIP01':
            bpy.ops.mesh.primitive_cylinder_add(radius=0.008, depth=1.0, vertices=8)
            o = bpy.context.object; o.data.materials.append(smat); o.rotation_mode = 'QUATERNION'
            sticks.append((n.parent, i, o))
    sc.display.shading.show_xray = True; sc.display.shading.xray_alpha = 0.55

def key_sticks(W, frame):
    from mathutils import Vector
    for a, b, o in sticks:
        pa, pb_ = P(W[a][:3, 3]), P(W[b][:3, 3])
        d = pb_ - pa; L = float(np.linalg.norm(d))
        o.location = tuple((pa + pb_) / 2); o.scale = (1, 1, max(L, 1e-4))
        o.rotation_quaternion = Vector((0, 0, 1)).rotation_difference(Vector(tuple(d / max(L, 1e-9))))
        for path in ('location', 'scale', 'rotation_quaternion'):
            o.keyframe_insert(path, frame=frame)

cams = {}
for name in ('side', 'front'):
    cam = bpy.data.cameras.new(name); cam.type = 'ORTHO'; cam.ortho_scale = 2.3
    o = bpy.data.objects.new(name, cam); sc.collection.objects.link(o); cams[name] = o

info = {'macro': {k_: macro[k_] for k_ in ('muscle', 'weight', 'proportions', 'height')}, 'scale_m_per_cm': k, 'clips': {}}
for clip_path in CLIPS:
    anim = ga.read_animation(clip_path)
    name = clip_path.name.split('-', 1)[1].split('.')[0]
    rig.animation_data_clear()
    for _, _, o in sticks:
        o.animation_data_clear()
    frames = anim.frame_count * (LOOPS if name.startswith('S_') else 1)
    travel = ga.root_travel(anim)
    knee = []
    for f in range(frames):
        W = ga.world_pose(sk, anim, f % anim.frame_count)
        if f >= anim.frame_count:                 # loops walk on from where the last one ended
            shift = np.eye(4); shift[:3, 3] = travel[-1] * (f // anim.frame_count)
            W = np.array([shift @ w for w in W])
        pose = solve(W)
        key(pose, f + 1)
        key_sticks(W, f + 1)
        pel = pose['pelvis'][:3, 3]
        for nm, o in cams.items():
            if nm == 'side':
                o.location = (pel[0] + 6, pel[1], 0.95); o.rotation_euler = (math.radians(90), 0, math.radians(90))
            else:
                o.location = (pel[0] - 4.2, pel[1] - 4.2, 1.0); o.rotation_euler = (math.radians(90), 0, math.radians(-45))
            o.keyframe_insert('location', frame=f + 1)
        knee.append(float(pose['calf_l'][2, 3]))
    sc.frame_start, sc.frame_end = 1, frames
    for nm, o in cams.items():
        sc.camera = o
        d = OUT / f'{name}-{nm}'; d.mkdir(exist_ok=True)
        sc.render.filepath = str(d / 'f_####')
        bpy.ops.render.render(animation=True)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-framerate', '25', '-i', str(d / 'f_%04d.png'),
                        '-pix_fmt', 'yuv420p', '-c:v', 'libx264', '-crf', '20', str(OUT / f'{name}-{nm}.mp4')], check=True)
    info['clips'][name] = {'frames': frames, 'knee_height_m': [min(knee), max(knee)]}
    print('CLIP', name, frames)
json.dump(info, open(OUT / 'pilot.json', 'w'), indent=1)
bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'pilot.blend'))
print('PILOT', json.dumps(info))
