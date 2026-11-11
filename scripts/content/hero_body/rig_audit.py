"""Audit of the render rig over many Gothic clips: where our skeleton's pose leaves Gothic's in the places the world
touches - the hands (ladder rungs, tools, two-handed grips), the pelvis (seats, beds), the head (helmets, gaze) and the
feet on the ground - plus the knee against Gothic's own joint turns (reports/engine/2026-11-modern-skeleton, «Аудит»).

    uv run -q --with numpy python -I rig_audit.py <models dir of the base> <content dir> <CLIP>[,<CLIP>...] [out.json]

The rig as the engine has it: v5 legs (root heel) with rig_parts.SOFT, the clavicle rhythm of rig_parts.SHOULDER;
LEGS=v4 for the knees without the heel, LEGS=v2 for the lift of «Ноги»; ARMS=1 with HANDS=hold,world (0..1) for the arms' IK as the game sets it.
Per clip, maxima over the frames, cm (world, Gothic's units):
- hand:  our wrist from Gothic's wrist, the worse hand;
- grip:  the change of the distance between the two wrists (a two-handed hold opens or closes by this much);
- hold:  the left wrist in the right hand's frame, ours against Gothic's (the left hand slips off a 2H hilt by this);
- pelvis_y: our pelvis over Gothic's (a seat or a bed: the body floats or sinks by this much);
- head:  our head from Gothic's head node;
- knee:  the knee's flex against Gothic's own joint turns (FK), max |deg|, and its mean over the clip;
- plane: the side our knee bends to against the side Gothic's knee does, max deg where Gothic's knee is bent over 25;
- ball:  our ball of the foot against Gothic's on the ground (height range, cm) and its slide per frame;
- skid:  how far our foot moves along the ground in a frame while Gothic's stands: our ball where Gothic's ball is
         low (within 3 cm of the lowest the feet come in the clip) and still (under 0.7 cm a frame), else our heel
         where the point under Gothic's ankle is - v5 does not keep our ball on Gothic's, so `slide` no longer tells
         a foot that slips; `sink`: the lower end of our feet under the lower end of Gothic's;
- bob:   the largest change of the pelvis' height between frames beyond Gothic's own (the lift's jolt).
LEGS=v5: the pelvis' lift keeps a state between frames (rig_solve.Rig.knee_lift_v5), reset at each clip.
"""
import json, os, sys
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent / 'anim'))
import gothic_anim as ga
import rig_solve as rs
import rig_parts as rp


LEGS = os.environ.get('LEGS', 'v5')   # the legs variant of rig_solve.py (v5: the engine's root heel; v4: knees alone)


def audit(rig, anim):
    ri, gi = rig.ri, rig.gi
    hands = [('hand_l', 'BIP01 L HAND'), ('hand_r', 'BIP01 R HAND')]
    worst = dict(hand=0.0, hand_at=0, grip=0.0, pelvis_y=[1e9, -1e9], head=0.0)
    g_pel, g_head = gi['BIP01 PELVIS'], gi['BIP01 HEAD']
    rig.reset(); rig.dt = 1.0 / anim.fps
    ends, lifts = [], []
    for f in range(anim.frame_count):
        W = ga.world_pose(rig.sk, anim, f)
        P = rig.solve(W, LEGS)
        lifts.append(float(P[ri['pelvis']][1, 3] - W[g_pel][1, 3]))
        row = []
        for L in rig.legs:
            foot = P[L['foot']]
            ball = (foot @ rig.local[L['ball']])[:3, 3]
            heel = foot[:3, 3] + foot[:3, :3] @ rig.Rb[L['foot']][:3, :3].T @ np.array(
                [0.0, rig.Rb[L['ball']][1, 3] - rig.Rb[L['foot']][1, 3], -4.0])
            rot = W[L['sf']][:3, :3] @ rig.Sb[L['sf']][:3, :3].T
            gball = (W[L['sf']] @ L['ball_foot'])[:3]
            gheel = W[L['sf']][:3, 3] + rot @ np.array([0.0, rig.Rb[L['ball']][1, 3] - rig.Sb[L['sf']][1, 3], 0.0])
            row.append((ball, heel, gball, gheel))
        ends.append(row)
        for ours, gothic in hands:
            d = float(np.linalg.norm(P[ri[ours]][:3, 3] - W[gi[gothic]][:3, 3]))
            if d > worst['hand']:
                worst['hand'], worst['hand_at'] = d, f
        go = np.linalg.norm(W[gi['BIP01 L HAND']][:3, 3] - W[gi['BIP01 R HAND']][:3, 3])
        us = np.linalg.norm(P[ri['hand_l']][:3, 3] - P[ri['hand_r']][:3, 3])
        worst['grip'] = max(worst['grip'], abs(float(us - go)))
        dy = float(P[ri['pelvis']][1, 3] - W[g_pel][1, 3])
        worst['pelvis_y'] = [min(worst['pelvis_y'][0], dy), max(worst['pelvis_y'][1], dy)]
        worst['head'] = max(worst['head'], float(np.linalg.norm(P[ri['head']][:3, 3] - W[g_head][:3, 3])))
        # the left wrist in the right hand's frame: a two-handed hold (2H hilt, bow, spell) slips by this much
        # (our hand's frame turned back to Gothic's axes: the rig's hand turns as Gothic's hand times `turn`)
        rel = lambda M, r, l, T: (M[r][:3, :3] @ T.T).T @ (M[l][:3, 3] - M[r][:3, 3])
        d = float(np.linalg.norm(rel(P, ri['hand_r'], ri['hand_l'], rig.turn[ri['hand_r']])
                                 - rel(W, gi['BIP01 R HAND'], gi['BIP01 L HAND'], np.eye(3))))
        worst['hold'] = max(worst.get('hold', 0.0), d)
        # how far the arm leaves Gothic's turns: the upper arm's direction against its FK direction
        for sd in 'lr':
            up, lo = ri[f'upperarm_{sd}'], ri[f'lowerarm_{sd}']
            fk = W[rig.source[up]][:3, :3] @ rig.turn[up] @ rig.local[lo][:3, 3]
            now = P[lo][:3, 3] - P[up][:3, 3]
            a = float(np.degrees(np.arccos(np.clip(fk @ now / np.linalg.norm(fk) / np.linalg.norm(now), -1, 1))))
            if a > worst.get('arm_dev', 0.0):
                worst['arm_dev'], worst['arm_dev_at'] = a, f
        # our balls of the feet under the lower of Gothic's (the ground Gothic stands on), cm
        gball = min((W[L['sf']] @ L['ball_foot'])[1] for L in rig.legs)
        sink = min(float(P[L['foot']][1, 3] + (P[L['foot']][:3, :3] @ rig.local[L['ball']][:3, 3])[1]) for L in rig.legs) - gball
        worst['sink'] = min(worst.get('sink', 0.0), sink)
        # the side the knee bends to against Gothic's, where Gothic's knee is bent enough to have a side
        for L in rig.legs:
            gh, gk, gf = (W[L[k]][:3, 3] for k in ('st', 'sk', 'sf'))
            hip, knee, ank = P[L['thigh']][:3, 3], P[L['calf']][:3, 3], P[L['foot']][:3, 3]
            if rs.flex(gh, gk, gf) < 25:
                continue
            off = lambda a, b, c: (b - a) - (c - a) / np.linalg.norm(c - a) * ((b - a) @ (c - a) / np.linalg.norm(c - a))
            o1, o2 = off(hip, knee, ank), off(gh, gk, gf)
            ang = float(np.degrees(np.arccos(np.clip(o1 @ o2 / np.linalg.norm(o1) / np.linalg.norm(o2), -1, 1))))
            if ang > worst.get('plane', 0.0):
                worst['plane'], worst['plane_at'] = ang, f
    floor = min(min(e[2][1], e[3][1]) for row in ends for e in row)
    worst['skid'] = 0.0
    for f in range(len(ends) - 1):
        for e0, e1 in zip(ends[f], ends[f + 1]):
            for ours, gothic in ((0, 2), (1, 3)):       # the ball, then the heel
                still = np.linalg.norm((e1[gothic] - e0[gothic])[[0, 2]]) < 0.7
                if still and e0[gothic][1] < floor + 3 and e1[gothic][1] < floor + 3:
                    worst['skid'] = max(worst['skid'], float(np.linalg.norm((e1[ours] - e0[ours])[[0, 2]])))
                    break
    worst['sink_ball'] = worst['sink']
    worst['sink'] = min(min(min(e[0][1], e[1][1]) for e in row) - min(min(e[2][1], e[3][1]) for e in row) for row in ends)
    worst['bob'] = float(np.abs(np.diff(lifts)).max()) if len(lifts) > 1 else 0.0
    rig.reset()
    rows = rs.measure(rig, anim, LEGS)
    dflex = np.array([x['flex'] - x['gflex'] for x in rows])
    c = [x for x in rows if x['contact']]
    worst.update(knee=float(np.abs(dflex).max()), knee_avg=float(dflex.mean()),
                 ball=[float(min(x['ball_dy'] for x in c)), float(max(x['ball_dy'] for x in c))] if c else None,
                 slide=float(max(x['slide'] for x in c)) if c else None, frames=anim.frame_count)
    return worst


if __name__ == '__main__':
    models, content = Path(sys.argv[1]), Path(sys.argv[2])
    clips = sys.argv[3].split(',')
    rig = rs.Rig(models, content)
    rig.lift, rig.share, rig.soft = 1.0, 0.038, rp.SOFT     # the share: v2's lift of «Ноги»
    rig.arm_soft = rp.ARM_SOFT
    rig.rhythm = (rp.SHOULDER['share'], rp.SHOULDER['from'], rp.SHOULDER['max'])
    rig.arms_ik = os.environ.get('ARMS', '') == '1'
    rig.hold, rig.world = (float(x) for x in os.environ.get('HANDS', '0,0').split(','))   # RenderRig::Hands
    out = {}
    print(f'{"clip":24s} {"fr":>4s} {"hand":>5s} {"@":>4s} {"grip":>5s} {"hold":>5s} {"pelvis_y":>11s} {"head":>5s} '
          f'{"knee":>5s} {"avg":>5s} {"plane":>5s} {"@":>4s} {"ball":>11s} {"slide":>5s} {"sink":>5s} {"arm":>5s} '
          f'{"skid":>5s} {"bob":>5s}')
    for clip in clips:
        anim = ga.read_animation(models / f'HUMANS-{clip}.MAN.json')
        w = audit(rig, anim)
        out[clip] = w
        ball = f'{w["ball"][0]:+5.1f}..{w["ball"][1]:+4.1f}' if w['ball'] else '-'
        slide = f'{w["slide"]:5.1f}' if w['slide'] is not None else '-'
        print(f'{clip:24s} {w["frames"]:4d} {w["hand"]:5.1f} {w["hand_at"]:4d} {w["grip"]:5.1f} {w["hold"]:5.1f} '
              f'{w["pelvis_y"][0]:+5.1f}..{w["pelvis_y"][1]:+4.1f} {w["head"]:5.1f} {w["knee"]:5.1f} '
              f'{w["knee_avg"]:+5.1f} {w.get("plane", 0):5.1f} {w.get("plane_at", 0):4d} {ball:>11s} {slide:>5s} '
              f'{w["sink"]:+5.1f} {w.get("arm_dev", 0):5.1f} {w["skid"]:5.1f} {w["bob"]:5.1f}')
    if len(sys.argv) > 4:
        Path(sys.argv[4]).write_text(json.dumps(out, indent=1))
