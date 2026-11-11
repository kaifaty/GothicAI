"""Probe: a Kimodo sample on the new body two ways - through Gothic's skeleton, as the game does, and straight onto
the body's own skeleton (reports/content/2026-11-hero-locomotion, «Проба: Kimodo сразу на новый скелет»).

    uv run -q --with numpy python -I rig_direct.py <base models> <content dir> <skin_standard.npz> <sample.npz> \
        <retargeted.MAN.json> <out dir>

- chain:  the record `retarget_soma.py` (and `knee_floor.py`) made of the sample on BIP01, carried onto the render
  rig by the engine's solve (`rig_solve.Rig.solve`, legs v5, as `rig_audit.py` sets it) and its twist bones;
- direct: the sample's joint turns put onto the rig's bones, the way `retarget_soma.py` puts them onto BIP01:
  SOMA's global rotations are turns from its T-pose, each mapped bone's bind orientation is turned by the shortest
  arc until the bone points as SOMA's does in that pose, W_b(t) = F R_s(t) F A_b. The rig keeps its bone lengths.
  The hips sit on SOMA's hip joints (same scale as the chain's, so both travel alike), raised by what the rig's leg
  is longer than SOMA's; the legs are solved onto SOMA's ankles; the lower ankle's 10th percentile stands at the
  bind ankle's height. Kimodo does not move the fingers (the 77-joint sample holds one relaxed hand in every
  frame), so the hand keeps the chain's fist; the twist bones are the same rule as the chain's.

Writes `<out>/chain.npz` and `<out>/direct.npz` for `blender/soma_view.py` (`"seq"`): the body's vertices in every
frame, cm in Gothic's frame, its triangles and the pelvis' path. Prints both against the sample's own joints
(`transfer_cues.signals`): ranges, and the mean and largest difference with the skeletons' constant offset out.
"""
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent / 'anim'))
sys.path.insert(0, str(HERE.parents[2] / '.claude' / 'skills' / 'own-animations' / 'scripts'))
import gothic_anim as ga
import retarget_soma as rs
import rig_parts as rp
import rig_solve
import transfer_cues as tc

F = rs.F
MAP = {  # rig bone: (SOMA joint, SOMA joint the bone points to, rig bone it points to)
    'pelvis': ('Hips', None, None), 'spine_01': ('Spine1', 'Spine2', 'spine_02'), 'spine_02': ('Spine2', 'Chest', 'spine_03'),
    'spine_03': ('Chest', 'Neck1', 'neck_01'), 'neck_01': ('Neck1', 'Head', 'head'), 'head': ('Head', None, None)}
for s, S in (('l', 'Left'), ('r', 'Right')):
    MAP.update({f'clavicle_{s}': (f'{S}Shoulder', f'{S}Arm', f'upperarm_{s}'), f'upperarm_{s}': (f'{S}Arm', f'{S}ForeArm', f'lowerarm_{s}'),
                f'lowerarm_{s}': (f'{S}ForeArm', f'{S}Hand', f'hand_{s}'), f'hand_{s}': (f'{S}Hand', f'{S}HandMiddle2', f'middle_01_{s}'),
                f'thigh_{s}': (f'{S}Leg', f'{S}Shin', f'calf_{s}'), f'calf_{s}': (f'{S}Shin', f'{S}Foot', f'foot_{s}'),
                f'foot_{s}': (f'{S}Foot', f'{S}ToeBase', f'ball_{s}'), f'ball_{s}': (f'{S}ToeBase', None, None)})


def mat(R, p):
    M = np.eye(4); M[:3, :3] = R; M[:3, 3] = p
    return M


def chain_pose(rig, anim):
    """The rig's world matrices in every frame of a BIP01 record, as the engine solves them."""
    rig.reset(); rig.dt = 1.0 / anim.fps
    out = []
    for f in range(anim.frame_count):
        P = rig.solve(ga.world_pose(rig.sk, anim, f), 'v5')
        for name, M in rig_solve.twist_bones(rig, P).items():
            P[rig.ri[name]] = M
        out.append(P)
    return np.array(out)


def direct_pose(rig, sample, skin, scale, fist):
    """The rig's world matrices from the sample's joints; `fist`: the chain's matrices, for the fingers."""
    rk, ri, Rb = rig.rk, rig.ri, rig.Rb
    names = [str(x) for x in skin['rig_joint_names']]
    si = {n: i for i, n in enumerate(names)}
    par = {names[c]: names[p] for p, c in skin['rig_joint_connections']}
    pos, rot = sample['posed_joints'].astype(float), sample['global_rot_mats'].astype(float)
    off = {n: rot[0, si[p]].T @ (pos[0, si[n]] - pos[0, si[p]]) for n, p in par.items()}    # SOMA's T-pose offsets

    def rest_dir(s, c):
        v = np.zeros(3)
        while c != s:
            v, c = v + off[c], par[c]
        return F @ v

    aligned = {}
    for b, (s, c, child) in MAP.items():
        r = Rb[ri[b]][:3, :3]
        if c:
            r = rs.shortest_arc(Rb[ri[child]][:3, 3] - Rb[ri[b]][:3, 3], rest_dir(s, c)) @ r
        aligned[b] = r
    local = [n.rest for n in rk.nodes]
    depth = lambda i: 0 if rk.nodes[i].parent < 0 else 1 + depth(rk.nodes[i].parent)
    order = sorted(range(len(rk.nodes)), key=depth)
    under = lambda i, a: rk.nodes[i].parent >= 0 and (rk.nodes[i].parent == a or under(rk.nodes[i].parent, a))
    pelvis, legs = ri['pelvis'], [(ri[f'thigh_{s}'], ri[f'calf_{s}'], ri[f'foot_{s}']) for s in 'lr']
    thigh_off = 0.5 * (local[legs[0][0]][:3, 3] + local[legs[1][0]][:3, 3])
    leg_len = lambda j, a, b, c: np.linalg.norm(j(b) - j(a)) + np.linalg.norm(j(c) - j(b))
    raise_y = leg_len(lambda n: Rb[ri[n]][:3, 3], 'thigh_l', 'calf_l', 'foot_l') \
        - scale * leg_len(lambda n: pos[0, si[n]], 'LeftLeg', 'LeftShin', 'LeftFoot')
    frames = len(pos)
    ankles = [np.array([F @ p * scale for p in pos[:, si[f'{S}Foot']]]) for S in ('Left', 'Right')]
    lift = Rb[legs[0][2]][1, 3] - float(np.percentile(np.minimum(ankles[0][:, 1], ankles[1][:, 1]), 10))
    out = np.zeros((frames, len(rk.nodes), 4, 4))
    for t in range(frames):
        R = np.zeros((len(rk.nodes), 3, 3))
        mapped = {ri[b]: F @ rot[t, si[s]] @ F @ aligned[b] for b, (s, _, _) in MAP.items()}

        def turns():
            for i in order:
                p = rk.nodes[i].parent
                R[i] = mapped[i] if i in mapped else (R[pelvis] @ local[pelvis][:3, :3].T if p < 0 else R[p] @ local[i][:3, :3])
        R[pelvis] = mapped[pelvis]
        turns()
        mid = F @ (0.5 * (pos[t, si['LeftLeg']] + pos[t, si['RightLeg']])) * scale + np.array([0.0, lift + raise_y, 0.0])
        root = mid - R[pelvis] @ thigh_off - R[0] @ local[pelvis][:3, 3]
        for k, chain in enumerate(legs):
            rs.two_bone_ik(R, root, rk, chain, ankles[k][t] + np.array([0.0, lift, 0.0]))
            mapped[chain[0]], mapped[chain[1]] = R[chain[0]].copy(), R[chain[1]].copy()
        turns()                                             # the thighs' twist bones follow the solved legs
        P = rs.world_positions(rk, R, root)
        M = np.array([mat(R[i], P[i]) for i in range(len(rk.nodes))])
        for s in 'lr':                                      # the chain's fist, carried by this hand
            hand = ri[f'hand_{s}']
            carry = M[hand] @ np.linalg.inv(fist[t, hand])
            for i in order:
                if under(i, hand):
                    M[i] = carry @ fist[t, i]
        for name, T in rig_solve.twist_bones(rig, M).items():
            M[ri[name]] = T
        out[t] = M
    return out


def body_mesh(path, rig):
    """Every mesh of the body's record (skin, braies) and a plain ball for the head, which is a mesh of its own in the
    game: bind positions, triangles and a dense weight table [vertices, bones]."""
    pos, tris, rows = [], [], []
    for k in range(len(json.loads(path.read_text())['data']['meshes'])):
        m = ga.read_skin_mesh(path, rig.rk, k)
        tris += (np.asarray(m.triangles) + sum(len(p) for p in pos)).tolist()
        pos.append(m.positions); rows += m.weights
    head = rig.ri['head']
    ring, seg, r = 8, 16, 10.5
    centre = rig.Rb[head][:3, 3] + rig.Rb[head][:3, :3] @ (rig.Rb[head][:3, :3].T @ np.array([0.0, 5.0, 2.0]))
    ball = [centre + r * np.array([np.sin(np.pi * i / ring) * np.cos(2 * np.pi * j / seg), np.cos(np.pi * i / ring),
                                   np.sin(np.pi * i / ring) * np.sin(2 * np.pi * j / seg)])
            for i in range(ring + 1) for j in range(seg)]
    base = sum(len(p) for p in pos)
    at = lambda i, j: base + i * seg + j % seg
    tris += [t for i in range(ring) for j in range(seg) for t in
             ((at(i, j), at(i + 1, j), at(i + 1, j + 1)), (at(i, j), at(i + 1, j + 1), at(i, j + 1)))]
    pos.append(np.array(ball)); rows += [[(head, 1.0)]] * len(ball)
    pos = np.concatenate(pos)
    W = np.zeros((len(pos), len(rig.Rb)))
    for v, ws in enumerate(rows):
        for n, w in ws:
            W[v, n] += w
    return pos, np.array(tris), W


def skin_frames(mesh, Rb, poses):
    pos, _, W = mesh
    bind = np.c_[pos, np.ones(len(pos))]
    inv_bind = np.linalg.inv(Rb)
    return np.array([np.einsum('vn,nij,vj->vi', W, P @ inv_bind, bind)[:, :3] for P in poses])


def rig_signals(rig, poses):
    pts = poses[:, :, :3, 3]
    return tc.signals(lambda n: pts[:, rig.ri[n]], 'pelvis', 'neck_01',
                      lambda s: [f'{n}_{s.lower()}' for n in ('thigh', 'calf', 'foot', 'ball', 'upperarm', 'lowerarm', 'hand')])


def main():
    models, content, skin_path, npz, rec, out = (Path(x) for x in sys.argv[1:7])
    rig = rig_solve.Rig(models, content)
    rig.lift, rig.share, rig.soft, rig.arm_soft = 1.0, 0.038, rp.SOFT, rp.ARM_SOFT           # as rig_audit.py
    rig.rhythm = (rp.SHOULDER['share'], rp.SHOULDER['from'], rp.SHOULDER['max'])
    rig.arms_ik, rig.hold, rig.world = False, 0.0, 0.0
    sample, skin = np.load(npz), np.load(skin_path)
    anim = ga.read_animation(rec)
    scale = float(np.linalg.norm(anim.positions[-1, 0, [0, 2]] - anim.positions[0, 0, [0, 2]])
                  / np.linalg.norm((sample['root_positions'][-1] - sample['root_positions'][0])[[0, 2]]))
    chain = chain_pose(rig, anim)
    direct = direct_pose(rig, sample, skin, scale, chain)
    mesh = body_mesh(content / 'models' / 'HUM_BODY_NAKED0.MDM.json', rig)
    out.mkdir(parents=True, exist_ok=True)
    for name, poses in (('chain', chain), ('direct', direct)):
        np.savez_compressed(out / f'{name}.npz', verts=skin_frames(mesh, rig.Rb, poses).astype(np.float32),
                            faces=mesh[1], root=poses[:, rig.ri['pelvis'], :3, 3], poses=poses)
    S, C, D = tc.soma_signals(npz), rig_signals(rig, chain), rig_signals(rig, direct)
    rng = lambda v: f'{v.min():7.1f}..{v.max():<7.1f}'
    print(f'scale {scale:.2f} cm per SOMA metre')
    print(f"{'':24}{'sample':>16}{'chain':>16}{'direct':>16}   chain: mean  max   direct: mean  max")
    for k in S:
        row = f'{k:24}{rng(S[k])}{rng(C[k])}{rng(D[k])}'
        for X in (C, D):
            m, x = tc.differ(S[k], X[k], not k.endswith('°') or k.startswith('lean'))
            row += f'{m:14.1f}{x:6.1f}'
        print(row)


if __name__ == '__main__':
    main()
