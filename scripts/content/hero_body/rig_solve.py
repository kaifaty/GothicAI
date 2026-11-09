"""The render rig's solve in numpy: a copy of RenderRig::solve (OpenGothic/common/graphics/mesh/renderrig.cpp) for
offline previews and measurements, plus the leg variants under study (reports/engine/2026-11-modern-skeleton,
«Ноги на беге»).

    uv run --with numpy python -I rig_solve.py <models dir of the base> <content dir> <CLIP> [...]   # knee/feet table

Variants of the legs (`legs=`):
- engine: as the engine does now - two-bone IK onto the ankle carried by Gothic's foot node, knee towards Gothic's knee;
- pole:   the same target, the knee towards the thigh's own front (the rig thigh's turn from its bind);
- shin:   pole + the ankle carried by Gothic's calf (a point of its shin), the foot turned onto Gothic's ball;
- fk:     no IK: the thigh, calf and foot take Gothic's turns like the other bones;
- v2:     what the engine does from «Ноги на беге»: the knee towards the rig's own bind knee carried by the thigh's
          turn, the pelvis raised by `share` of its height over the lower of Gothic's ankles (Gothic's clips are made
          for legs 2.6 cm shorter than its bind skeleton, ours are fitted to the bind), soft IK over the last `soft` cm.
"""
import json, math, sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'anim'))
import gothic_anim as ga

inv = np.linalg.inv
FWD = np.array([0.0, 0.0, 1.0])     # Gothic model frame: z ahead, y up


def rot_between(a, b):
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(a @ b)
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx / (1 + c)


class Rig:
    def __init__(self, models: Path, content: Path, name='HUM_RIG_MPFB'):
        self.sk = ga.read_skeleton(models / 'HUMANS.MDH.json')
        self.rk = ga.read_skeleton(content / 'models' / f'{name}.MDH.json')
        spec = json.load(open(content / 'models' / 'render-rigs.json'))['rigs'][name]
        sk, rk = self.sk, self.rk
        self.gi = {n.name: i for i, n in enumerate(sk.nodes)}
        self.ri = {n.name: i for i, n in enumerate(rk.nodes)}
        self.Sb = ga.world_pose(sk, None, 0)
        self.Rb = ga.world_pose(rk, None, 0)
        Sb, Rb = self.Sb, self.Rb
        n = len(rk.nodes)
        self.parent = [nd.parent for nd in rk.nodes]
        self.local = [inv(Rb[p]) @ Rb[i] if p >= 0 else inv(Sb[0]) @ Rb[i] for i, p in enumerate(self.parent)]
        self.source = [None] * n; self.turn = [None] * n
        for b in spec['bones']:
            if b.get('source'):
                i = self.ri[b['name']]; s = self.gi[b['source']]
                self.source[i] = s
                self.turn[i] = Sb[s][:3, :3].T @ ga.quat_matrix(np.array(b['align'], float)) @ Rb[i][:3, :3]
        self.root_b = self.ri[spec['root']['bone']]; self.root_s = self.gi[spec['root']['source']]
        self.root_local = inv(Sb[self.root_s]) @ np.r_[Rb[self.root_b][:3, 3], 1]
        self.legs, self.role = [], {}
        for k, l in enumerate(spec['legs']):
            L = dict(thigh=self.ri[l['thigh']], calf=self.ri[l['calf']], foot=self.ri[l['foot']], ball=self.ri[l['ball']],
                     sf=self.gi[l['source_foot']], sk=self.gi[l['source_knee']], ankle=np.r_[l['ankle'], 1.0],
                     side=l['thigh'][-1])
            S = L['side'].upper()
            L['st'] = self.gi[f'BIP01 {S} THIGH']; L['stoe'] = self.gi[f'BIP01 {S} TOE0']
            # the ankle as a point of Gothic's shin: the same bind point in the calf's frame
            L['ankle_calf'] = inv(Sb[L['sk']]) @ (Sb[L['sf']] @ L['ankle'])
            # the rig's ball as a point of Gothic's foot (bind): where Gothic carries it
            L['ball_foot'] = inv(Sb[L['sf']]) @ np.r_[Rb[L['ball']][:3, 3], 1]
            # v2: the knee's front - the model's front in the bind pose, in the thigh's frame (not the bind knee's own
            # offset: MPFB's legs stand apart and its knee leans 11 degrees in off their line, which knocks the knees)
            L['front'] = Rb[L['thigh']][:3, :3].T @ FWD
            self.legs.append(L)
            self.role[L['thigh']] = ('T', k); self.role[L['calf']] = ('C', k); self.role[L['foot']] = ('F', k)
        depth = lambda i: 0 if self.parent[i] < 0 else 1 + depth(self.parent[i])
        self.order = sorted(range(n), key=depth)

    def place(self, i, W, out):
        p = self.parent[i]
        m = (out[p] if p >= 0 else W[0]) @ self.local[i]
        if self.source[i] is not None:
            m[:3, :3] = W[self.source[i]][:3, :3] @ self.turn[i]
        if i == self.root_b:
            m[:3, 3] = (W[self.root_s] @ self.root_local)[:3]
            m[1, 3] *= self.lift               # the pelvis' height over the ground scaled to our legs
            m[1, 3] += self.dy                 # v2: the pelvis raised by a share of its height over the lower ankle
        return m

    def leg(self, L, W, out, legs):
        th = out[L['thigh']]; hip = th[:3, 3].copy()
        if legs == 'fk':
            out[L['calf']] = self.place(L['calf'], W, out)
            out[L['foot']] = self.place(L['foot'], W, out)
            return
        toCalf = self.local[L['calf']][:3, 3]; toFoot = self.local[L['foot']][:3, 3]
        a1 = th[:3, :3] @ toCalf
        out[L['calf']] = self.place(L['calf'], W, out)
        a2 = out[L['calf']][:3, :3] @ toFoot
        l1, l2 = np.linalg.norm(a1), np.linalg.norm(a2)
        if legs == 'shin':
            target = (W[L['sk']] @ L['ankle_calf'])[:3]
        else:
            target = (W[L['sf']] @ L['ankle'])[:3]
        tt = target - hip; dist = np.linalg.norm(tt); u = tt / dist
        d = min(max(dist, abs(l1 - l2) + 1e-6), l1 + l2)
        if self.soft > 0:                    # soft IK: the last few cm of reach come slowly, no knee snap
            ds = l1 + l2 - self.soft
            if dist > ds:
                d = ds + self.soft * (1 - math.exp(-(dist - ds) / self.soft))
        along = (l1 * l1 - l2 * l2 + d * d) / (2 * d); up = math.sqrt(max(l1 * l1 - along * along, 0))
        end = hip + u * d
        if legs == 'v2':
            front = th[:3, :3] @ L['front']
            side = front - u * (front @ u)
        elif legs == 'engine':
            pole = W[L['sk']][:3, 3] - hip
            side = pole - u * (pole @ u)
            if np.linalg.norm(side) < 1e-4:
                side = a1 - u * (a1 @ u)
        else:
            # the knee's front: the rig thigh's turn from its bind applied to the model's front
            front = th[:3, :3] @ self.Rb[L['thigh']][:3, :3].T @ FWD
            side = front - u * (front @ u)
        side = side / np.linalg.norm(side)
        knee = hip + u * along + side * up
        th[:3, :3] = rot_between(a1, knee - hip) @ th[:3, :3]
        calf = self.place(L['calf'], W, out); calf[:3, 3] = knee
        calf[:3, :3] = rot_between(calf[:3, :3] @ toFoot, end - knee) @ calf[:3, :3]
        out[L['calf']] = calf
        foot = self.place(L['foot'], W, out); foot[:3, 3] = end
        if legs == 'shin':
            # the foot's turn is Gothic's; then aimed so the ball lands where Gothic's foot carries it
            ball_now = foot @ self.local[L['ball']]
            want = (W[L['sf']] @ L['ball_foot'])[:3]
            foot[:3, :3] = rot_between(ball_now[:3, 3] - end, want - end) @ foot[:3, :3]
        out[L['foot']] = foot

    lift = 1.0
    soft = 0.0
    share = 0.0      # v2 lift: the share of the pelvis' height over the lower of Gothic's ankles
    dy = 0.0

    def solve(self, W, legs='engine'):
        out = [None] * len(self.parent)
        self.dy = 0.0
        if self.share > 0 and self.legs:
            pel = (W[self.root_s] @ self.root_local)[1]
            low = min((W[L['sf']] @ L['ankle'])[1] for L in self.legs)
            self.dy = self.share * max(0.0, pel - low)
        for i in self.order:
            r = self.role.get(i)
            if r and r[0] in 'CF':
                continue
            out[i] = self.place(i, W, out)
            if r and r[0] == 'T':
                self.leg(self.legs[r[1]], W, out, legs)
        return np.array(out)


def flex(a, b, c):
    v1, v2 = a - b, c - b
    return 180 - math.degrees(math.acos(np.clip(v1 @ v2 / np.linalg.norm(v1) / np.linalg.norm(v2), -1, 1)))


def measure(rig: Rig, anim, legs):
    """Per leg and frame: knee flex against Gothic's, the knee's sideways offset, the ball's height and slide."""
    rows = []
    prev = {}
    for f in range(anim.frame_count):
        W = ga.world_pose(rig.sk, anim, f)
        P = rig.solve(W, legs)
        keep = rig.lift, rig.share
        rig.lift, rig.share = 1.0, 0.0
        F = rig.solve(W, 'fk'); rig.lift, rig.share = keep
        for L in rig.legs:
            s = L['side']; out_dir = np.array([-1.0 if s == 'l' else 1.0, 0, 0])
            gh, gk, gf = (W[L[k]][:3, 3] for k in ('st', 'sk', 'sf'))
            hip, knee, ank = P[L['thigh']][:3, 3], P[L['calf']][:3, 3], P[L['foot']][:3, 3]
            u = (ank - hip) / np.linalg.norm(ank - hip)
            off = (knee - hip) - u * ((knee - hip) @ u)
            ball = (P[L['foot']] @ rig.local[L['ball']])[:3, 3]
            gball = (W[L['sf']] @ L['ball_foot'])[:3]
            contact = gball[1] < 4.0
            slide = 0.0
            if contact and s in prev and prev[s][2]:
                slide = np.linalg.norm(((ball - gball) - (prev[s][0] - prev[s][1]))[[0, 2]])
            prev[s] = (ball, gball, contact)
            fflex = flex(F[L['thigh']][:3, 3], F[L['calf']][:3, 3], F[L['foot']][:3, 3])
            rows.append(dict(f=f, side=s, gflex=fflex, flex=flex(hip, knee, ank), inward=-(off @ out_dir),
                             ball_dy=ball[1] - gball[1], contact=contact, slide=slide))
    return rows


def summary(rows):
    r = rows
    dflex = np.array([x['flex'] - x['gflex'] for x in r])
    inward = np.array([x['inward'] for x in r])
    c = [x for x in r if x['contact']]
    dy = np.array([x['ball_dy'] for x in c]) if c else np.zeros(1)
    sl = np.array([x['slide'] for x in c]) if c else np.zeros(1)
    return (f'knee flex vs FK {dflex.mean():+5.1f} avg, {np.abs(dflex).max():5.1f} max deg | knee inward max {inward.max():5.1f} cm'
            f' | ball on ground: height {dy.min():+5.1f}..{dy.max():+5.1f} cm, slide max {sl.max():4.1f} cm/frame')


if __name__ == '__main__':
    models, content = Path(sys.argv[1]), Path(sys.argv[2])
    rig = Rig(models, content)
    VARIANTS = [('engine', 0, 0), ('v2', 0.036, 0.5), ('v2', 0.038, 0.5), ('v2', 0.040, 0.5)]
    for clip in sys.argv[3:]:
        anim = ga.read_animation(models / f'HUMANS-{clip}.MAN.json')
        print(clip, anim.frame_count, 'frames')
        for legs, rig.share, rig.soft in VARIANTS:
            print(f'  {legs:6s} lift {rig.share:.3f} soft {rig.soft:.1f}', summary(measure(rig, anim, legs)))
