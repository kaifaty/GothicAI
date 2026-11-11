"""The render rig's solve in numpy: a copy of RenderRig::solve (OpenGothic/common/graphics/mesh/renderrig.cpp) for
offline previews and measurements, plus the leg variants under study (reports/engine/2026-11-modern-skeleton,
«Ноги»).

    uv run --with numpy python -I rig_solve.py <models dir of the base> <content dir> <CLIP> [...]   # knee/feet table

Variants of the legs (`legs=`):
- engine: as the engine does now - two-bone IK onto the ankle carried by Gothic's foot node, knee towards Gothic's knee;
- pole:   the same target, the knee towards the thigh's own front (the rig thigh's turn from its bind);
- shin:   pole + the ankle carried by Gothic's calf (a point of its shin), the foot turned onto Gothic's ball;
- fk:     no IK: the thigh, calf and foot take Gothic's turns like the other bones;
- v2:     what the engine does since «Ноги»: the knee towards the model's front of the bind pose carried by the
          thigh's turn (as pole), the pelvis raised by `share` of its height over the lower of Gothic's ankles (Gothic's clips are made
          for legs 2.6 cm shorter than its bind skeleton, ours are fitted to the bind), soft IK over the last `soft` cm.
- v3:     a step to v4 (the pelvis by the knee's reach, a foot in the air from Gothic's shin); kept for the tables;
- v4:     what the engine does with root knees («Аудит»): the pelvis at the height where the carrying legs reach their
          feet at Gothic's knee angle (knee_lift_v4), a foot on the ground pivots about Gothic's ball, one in the air
          takes Gothic's turns (contact_target);
- v5:     what the engine does with root heel («Пятка»): v4 with a leg that stands on its foot standing heel on heel
          with Gothic's (pivot), the pelvis' lift and the legs' shares kept between solves (knee_lift_v5; reset()
          at each clip, dt - the frame's seconds), a leg on the ground no straighter than Gothic's knee (leg).
Arms («Аудит»): arms_ik with hold / world - RenderRig::solveArm.
Upper body («Верх тела»): `Rig.rhythm` turns the clavicles with the arm's rise; `twist_bones()` gives the twist bones of
rig_parts.TWISTS for a solved pose and `twist_of()` measures a bone's twist.
"""
import json, math, sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'anim'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga
from rig_parts import TWISTS  # the twist bones (rig_parts.py)

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
        self.arms = []
        for sd in 'lr':
            S = sd.upper()
            A = dict(side=sd, up=self.ri[f'upperarm_{sd}'], lo=self.ri[f'lowerarm_{sd}'], hand=self.ri[f'hand_{sd}'],
                     sh=self.gi[f'BIP01 {S} HAND'])
            # the elbow's back: the model's back in the bind pose, in the upper arm's frame (the pole of a straight arm)
            A['back'] = Rb[A['up']][:3, :3].T @ -FWD
            self.arms.append(A)
        self.names = [nd.name for nd in rk.nodes]
        depth = lambda i: 0 if self.parent[i] < 0 else 1 + depth(self.parent[i])
        self.order = sorted(range(n), key=depth)

    rhythm = None    # (share, from, max degrees): the clavicle rises with the arm above `from` by `share` of the excess

    def place(self, i, W, out):
        p = self.parent[i]
        m = (out[p] if p >= 0 else W[0]) @ self.local[i]
        if self.source[i] is not None:
            m[:3, :3] = W[self.source[i]][:3, :3] @ self.turn[i]
        if self.rhythm and self.names[i].startswith('clavicle_'):
            m[:3, :3] = self.shoulder(i, W, out, m) @ m[:3, :3]
        if i == self.root_b:
            m[:3, 3] = (W[self.root_s] @ self.root_local)[:3]
            m[1, 3] *= self.lift               # the pelvis' height over the ground scaled to our legs
            m[1, 3] += self.dy                 # v2: the pelvis raised by a share of its height over the lower ankle
        return m

    def shoulder(self, i, W, out, m):
        """Scapulohumeral rhythm: the extra turn of clavicle i towards its arm, from the arm's rise in the chest."""
        sd = self.names[i][-1]; ri, Rb = self.ri, self.Rb
        up, lo, ch = ri[f'upperarm_{sd}'], ri[f'lowerarm_{sd}'], ri['spine_03']
        R_up = W[self.source[up]][:3, :3] @ self.turn[up]
        arm = R_up @ Rb[up][:3, :3].T @ (Rb[lo][:3, 3] - Rb[up][:3, 3]); arm /= np.linalg.norm(arm)
        down = out[ch][:3, :3] @ Rb[ch][:3, :3].T @ np.array([0, -1.0, 0])
        rise = math.degrees(math.acos(np.clip(arm @ down, -1, 1)))
        share, start = self.rhythm[:2]
        cap = self.rhythm[2] if len(self.rhythm) > 2 else 90.0
        extra = math.radians(min(cap, share * max(0.0, rise - start)))
        clav = m[:3, :3] @ Rb[i][:3, :3].T @ (Rb[up][:3, 3] - Rb[i][:3, 3]); clav /= np.linalg.norm(clav)
        ax = np.cross(clav, arm)
        if np.linalg.norm(ax) < 1e-6 or extra <= 0:
            return np.eye(3)
        extra = min(extra, math.acos(np.clip(clav @ arm, -1, 1)))
        return rot_axis(ax / np.linalg.norm(ax), extra)

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
        elif legs == 'v4':
            target, pivot = self.contact_target(L, W, hip, a1 + a2)
        elif legs == 'v3':
            # a foot on the ground goes where Gothic's foot carries the ankle; a foot in the air follows Gothic's shin,
            # so the knee does not jump when Gothic's foot turns about its pivot (5 cm above, 9 cm behind the ankle)
            c = self.contact[L['side']]
            target = c * (W[L['sf']] @ L['ankle'])[:3] + (1 - c) * (W[L['sk']] @ L['ankle_calf'])[:3]
        else:
            target = (W[L['sf']] @ L['ankle'])[:3]
        tt = target - hip; dist = np.linalg.norm(tt); u = tt / dist
        d = min(max(dist, abs(l1 - l2) + 1e-6), l1 + l2)
        if self.heel:                        # v5: no straighter than Gothic's knee: the foot stops short of a far target
            dist = min(dist, max(np.linalg.norm(a1 + a2), abs(l1 - l2) + 1e-6))
            d = min(d, dist)
        if self.soft > 0:                    # soft IK: the last few cm of reach come slowly, no knee snap
            ds = l1 + l2 - self.soft
            if dist > ds:
                d = ds + self.soft * (1 - math.exp(-(dist - ds) / self.soft))
        along = (l1 * l1 - l2 * l2 + d * d) / (2 * d); up = math.sqrt(max(l1 * l1 - along * along, 0))
        end = hip + u * d
        if legs in ('v2', 'v3', 'v4'):
            front = th[:3, :3] @ L['front']
            side = front - u * (front @ u)
            if legs == 'v4':                     # in the air the knee goes to the side Gothic's knee bends to
                c = self.contact[L['side']]; fks = a1 - u * (a1 @ u)
                if self.heel and fks @ side < 0:     # v5: Gothic's knee bent backwards - ours bends to the front
                    fks = np.zeros(3)
                if np.linalg.norm(fks) > 0.05 * l1:
                    side = c * side / max(np.linalg.norm(side), 1e-9) + (1 - c) * fks / np.linalg.norm(fks)
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
        if legs == 'v4' and pivot is not None:     # the foot turned about the ball by the contact's pitch
            foot[:3, :3] = pivot @ foot[:3, :3]
        if legs == 'shin':
            # the foot's turn is Gothic's; then aimed so the ball lands where Gothic's foot carries it
            ball_now = foot @ self.local[L['ball']]
            want = (W[L['sf']] @ L['ball_foot'])[:3]
            foot[:3, :3] = rot_between(ball_now[:3, 3] - end, want - end) @ foot[:3, :3]
        out[L['foot']] = foot

    pitch = (0.0, 50.0)  # v4: degrees the foot may turn about the ball beyond Gothic's: heel down to Gothic's flat, heel up
    contact_cm = (2.0, 8.0)   # v4: Gothic's ball over the lowest ball: on the ground below the first, in the air above

    def contact_target(self, L, W, hip, fk):
        """v4: where the ankle goes. On the ground the ball stays on Gothic's ball and the foot turns about it (heel up or
        back down to Gothic's flat foot) until the hip-ankle distance is the one of our leg in Gothic's knee angle; in the
        air the leg takes Gothic's turns as they are (FK). Returns the target and the foot's extra turn."""
        B, r, k, lo_, hi_, c = self.pivot(L, W)
        self.contact[L['side']] = c
        reach = np.linalg.norm(fk)
        rpar = (k @ r) * k; rperp = r - rpar; p = B - hip + rpar
        Ac, Bs = 2 * p @ rperp, 2 * p @ np.cross(k, rperp)
        C = reach ** 2 - p @ p - rperp @ rperp
        R = math.hypot(Ac, Bs); base = math.atan2(Bs, Ac)
        if R < 1e-9:
            th = 0.0
        elif abs(C) <= R:
            th = min((base + math.acos(C / R), base - math.acos(C / R)), key=lambda t: abs(math.remainder(t, 2 * math.pi)))
            th = math.remainder(th, 2 * math.pi)
        else:
            th = base if C > R else base + math.pi
            th = math.remainder(th, 2 * math.pi)
        th = float(np.clip(th, lo_, hi_))
        th *= self.carrying.get(L['side'], 0.0)     # a foot that does not carry the body (lying, sitting) stays as Gothic's
        ankle = B + rot_axis(k, th) @ r
        hipfk = hip + fk
        target = c * ankle + (1 - c) * hipfk
        return target, rot_axis(k, c * th)

    def ankle_of(self, L, W):
        """v3: Gothic's ankle point; v4: our ankle under Gothic's ball, the foot turned as Gothic's."""
        if self.v4:
            B, r = self.pivot(L, W)[:2]
            return B + r
        return (W[L['sf']] @ L['ankle'])[:3]

    v4 = False

    heel = False     # v5: the foot stands where Gothic's heel does (pivot)
    thigh_down = (0.5, 0.8)  # v5: the thigh's downward share from which a leg starts to stand on its heel / fully does
    shin_down = (0.2, 0.5)   # v5: the same for the shin
    plant_tau = 0.3  # v5: seconds the heel's share takes to follow the pose
    plant_prev = {}
    toes_fade = (10.0, 25.0)  # v5: degrees of toes up over which the heel's own lift fades out
    roll = (math.cos(math.radians(25)), math.cos(math.radians(45)))   # v5: Gothic's heel lift (its cosine) up to which
                     # our foot rolls over its own ball, and from which it leaves the ground with Gothic's heel

    def foot_low(self, M, W):
        """The lowest of the foot's two ends on Gothic's foot: our ball and, v5, the point under Gothic's ankle."""
        B = (W[M['sf']] @ M['ball_foot'])[:3]
        if not self.heel:
            return B[1]
        rot = W[M['sf']][:3, :3] @ self.Sb[M['sf']][:3, :3].T
        under = W[M['sf']][:3, 3] + rot @ np.array([0.0, self.Rb[M['ball']][1, 3] - self.Sb[M['sf']][1, 3], 0.0])
        return min(B[1], under[1])

    def pivot(self, L, W):
        """v4: the foot's pivot on the ground - Gothic's ball B, the ankle's offset r from it, the axis k (+ lifts the heel),
        the turn range lo..hi (rad) about it and how much the foot is on the ground (0..1).
        v5: our foot is shorter than Gothic's (ankle to ball 14 cm against 23), so with the balls together our ankle
        stands 9 cm ahead of Gothic's and the leg vaults over another point: the pelvis that keeps Gothic's knee rises
        through the stance and drops at the next step. The foot stands heel on heel instead: the pivot is Gothic's ball
        moved back by that difference - along the ground while the heel is up (the foot rolls over its own ball, which
        stays put), along the foot while the toes are up (the foot turns about Gothic's ankle, no heel lift of its
        own). On the ground: the lower of the ball and the point under Gothic's ankle is near the lowest one."""
        Mf = W[L['sf']]
        B = (Mf @ L['ball_foot'])[:3]
        rot = Mf[:3, :3] @ self.Sb[L['sf']][:3, :3].T
        fwd = rot @ FWD
        up = 0.0                                 # degrees the toes point up
        off = 1.0                                # v5: 0 - the heel is so high that the foot is leaving the ground
        if self.heel:
            delta = self.Rb[L['foot']][2, 3] - self.Sb[L['sf']][2, 3]   # bind: our ankle ahead of Gothic's
            g = fwd
            if fwd[1] < 0:
                flat = np.array([fwd[0], 0.0, fwd[2]]); n = np.linalg.norm(flat)
                s = float(np.clip((n - 0.2) / 0.3, 0, 1))        # a foot on end has no way along the ground
                off = float(np.clip((n - self.roll[1]) / (self.roll[0] - self.roll[1]), 0, 1))
                g = fwd * (1 - s) + flat / max(n, 1e-9) * s
            else:
                up = math.degrees(math.asin(np.clip(fwd[1], -1, 1)))
            # only a leg that stands on its foot: one that does not point down (lying), whose thigh is level (a
            # seat, a squat: the pelvis is held by something else) or whose shin is (kneeling) keeps ball on ball,
            # as v4 has it. The share follows the pose over `plant_tau` seconds and only while the foot is near the
            # ground: a lunge that levels the thigh for a moment does not drag the foot, a leg swung through the
            # air comes down as it left
            leg = Mf[:3, 3] - W[L['st']][:3, 3]; thigh = W[L['sk']][:3, 3] - W[L['st']][:3, 3]
            shin = Mf[:3, 3] - W[L['sk']][:3, 3]
            ramp = lambda v, a, b: float(np.clip((-v[1] / max(np.linalg.norm(v), 1e-9) - a) / (b - a), 0, 1))
            share = min(ramp(leg, *self.carry), ramp(thigh, *self.thigh_down), ramp(shin, *self.shin_down))
            if L['side'] not in self.planted:           # once a solve: pivot is asked several times
                was = self.plant_prev.get(L['side'])
                near = float(np.clip(1 - (self.foot_low(L, W) - min(self.foot_low(M, W) for M in self.legs) - self.contact_cm[0])
                                     / (self.contact_cm[1] - self.contact_cm[0]), 0, 1))
                self.planted[L['side']] = share if was is None else \
                    was + (share - was) * (1 - math.exp(-self.dt / self.plant_tau)) * near
            share = self.planted[L['side']]
            B = B - delta * g * share
            off = 1 - share * (1 - off)                 # a heel up on a leg that does not stand: kneeling
        # our ankle from our ball, the foot turned as Gothic's: the ball lands on the pivot exactly
        A0 = B - W[self.source[L['foot']]][:3, :3] @ self.turn[L['foot']] @ self.local[L['ball']][:3, 3]
        low = min(self.foot_low(M, W) for M in self.legs)
        c0, c1 = self.contact_cm
        c = off * float(np.clip(1 - (self.foot_low(L, W) - low - c0) / (c1 - c0), 0, 1))
        k = rot @ np.array([1.0, 0, 0])
        r = A0 - B
        if (rot_axis(k, 0.01) @ r)[1] < r[1]:   # +theta lifts the heel
            k = -k
        phi = math.degrees(math.asin(np.clip(-fwd[1], -1, 1)))          # Gothic's heel lift from its bind
        own = self.pitch[1] * float(np.clip(1 - (up - self.toes_fade[0]) / (self.toes_fade[1] - self.toes_fade[0]), 0, 1))
        lo_, hi_ = math.radians(-max(0.0, phi) - self.pitch[0]), math.radians(own - max(0.0, phi))
        self.match[L['side']] = 0.0
        if self.heel and phi > 0:
            # Gothic's ankle rises on a longer foot: ours turns further for the same rise, as far as it may
            lever = np.linalg.norm((Mf @ L['ball_foot'])[:3] - Mf[:3, 3]) / max(np.linalg.norm(r), 1e-9)
            want = math.asin(min(1.0, lever * math.sin(math.radians(phi)))) - math.radians(phi)
            self.match[L['side']] = float(np.clip(want, 0.0, max(hi_, 0.0)))
        return B, r, k, lo_, hi_, c

    def knee_lift_v4(self, W):
        """v4: the pelvis height. Each foot on the ground can turn about its ball within its range, so each carrying leg
        allows a band of heights at which it reaches with Gothic's knee angle; the pelvis takes the v3 height moved into
        the bands' overlap, or the middle of the gap when they do not overlap."""
        pref = self.knee_lift(W)
        self.carrying = {L['side']: 0.0 for L in self.legs}
        pr = self.root_b
        Rp = W[self.source[pr]][:3, :3] @ self.turn[pr]
        pos = (W[self.root_s] @ self.root_local)[:3]
        lo_all, hi_all = -1e9, 1e9
        for L in self.legs:
            hip = pos + Rp @ (inv(self.Rb[pr]) @ self.Rb[L['thigh']])[:3, 3]
            a1 = W[self.source[L['thigh']]][:3, :3] @ self.turn[L['thigh']] @ self.local[L['calf']][:3, 3]
            a2 = W[self.source[L['calf']]][:3, :3] @ self.turn[L['calf']] @ self.local[L['foot']][:3, 3]
            reach = np.linalg.norm(a1 + a2)
            B, r, k, lo_, hi_, c = self.pivot(L, W)
            v0 = B + r - hip
            down = -v0[1] / max(np.linalg.norm(v0), 1e-9)
            carry = c * float(np.clip((down - self.carry[0]) / (self.carry[1] - self.carry[0]), 0, 1))
            self.carrying[L['side']] = carry
            if carry <= 0:
                continue
            ys = []
            for th in np.linspace(lo_, hi_, 9):
                v = B + rot_axis(k, th) @ r - hip
                h2 = reach ** 2 - v[0] ** 2 - v[2] ** 2
                if h2 >= 0:
                    ys.append(v[1] + math.sqrt(h2))
            if not ys:
                continue
            slack = (1 - carry) * 20.0
            lo_all = max(lo_all, min(ys) - slack); hi_all = min(hi_all, max(ys) + slack)
        if lo_all <= hi_all:
            dy = float(np.clip(pref, lo_all, hi_all))
        else:
            dy = 0.5 * (lo_all + hi_all)
        return float(np.clip(dy, -10.0, 10.0))

    tau = 0.08       # v5: seconds the pelvis' lift takes to follow (1 - 1/e)
    dt = 1.0 / 30    # v5: seconds since the last solve
    prev = None      # v5: the lift of the last solve; None - the first one

    def reset(self):
        self.prev = None
        self.plant_prev = {}

    def knee_lift_v5(self, W):
        """v5: the pelvis' lift over frames. Each leg near the ground and under the hip asks for the lift at which it
        reaches its foot with Gothic's knee, the foot turned up as far as Gothic's longer foot lifts the ankle
        (pivot); the legs are weighed by how low the ankle is, how much the leg stands and how much the foot is on
        the ground. With no leg on the ground the last lift holds while a leg points down (a jump, the flight of a
        run) and goes to 0 otherwise (lying). The lift follows its goal over `tau` seconds and is never under what a
        carrying leg needs with its foot flat: a pelvis too low bends a straight knee by 17 degrees a cm, one too high
        only lifts a foot off the ground (leg)."""
        self.carrying = {L['side']: 0.0 for L in self.legs}
        self.contact = {}
        pr = self.root_b
        Rp = W[self.source[pr]][:3, :3] @ self.turn[pr]
        pos = (W[self.root_s] @ self.root_local)[:3]
        legs = []
        for L in self.legs:
            hip = pos + Rp @ (inv(self.Rb[pr]) @ self.Rb[L['thigh']])[:3, 3]
            a1 = W[self.source[L['thigh']]][:3, :3] @ self.turn[L['thigh']] @ self.local[L['calf']][:3, 3]
            a2 = W[self.source[L['calf']]][:3, :3] @ self.turn[L['calf']] @ self.local[L['foot']][:3, 3]
            reach = np.linalg.norm(a1 + a2)
            B, r, k, lo_, hi_, c = self.pivot(L, W)
            self.contact[L['side']] = c

            def shift(th):
                v = B + rot_axis(k, th) @ r - hip
                h2 = reach ** 2 - v[0] ** 2 - v[2] ** 2
                return v[1] + math.sqrt(h2) if h2 >= 0 else None

            A = B + rot_axis(k, self.match[L['side']]) @ r
            v = A - hip
            stands = float(np.clip((-v[1] / max(np.linalg.norm(v), 1e-9) - self.carry[0]) / (self.carry[1] - self.carry[0]), 0, 1))
            ys = [y for y in (shift(th) for th in np.linspace(lo_, hi_, 9)) if y is not None]
            legs.append(dict(side=L['side'], ankle=A[1], wish=shift(self.match[L['side']]), stands=stands, c=c,
                             low=min(ys) if ys else None))
            self.carrying[L['side']] = c * stands
        low = min(l['ankle'] for l in legs)
        num = den = 0.0
        for l in legs:
            if l['wish'] is None:
                continue
            w = math.exp(-(l['ankle'] - low) / self.sigma) * l['stands'] * l['c']
            num += w * l['wish']; den += w
        trust = min(den, 1.0)
        goal = trust * num / max(den, 1e-6)
        if self.prev is None:
            dy = goal
        else:
            goal += (1 - trust) * max(l['stands'] for l in legs) * self.prev
            dy = self.prev + (goal - self.prev) * (1 - math.exp(-self.dt / self.tau))
        for l in legs:
            carry = self.carrying[l['side']]
            if carry > 0 and l['low'] is not None:
                dy = max(dy, l['low'] - (1 - carry) * 20.0)
        dy = float(np.clip(dy, -10.0, 10.0))
        self.prev = dy
        self.plant_prev = dict(self.planted)
        return dy

    arms_ik = False   # the arms of RenderRig::solveArm, by the game's hands (RenderRig::Hands, 0..1 each):
    hold = 0.0        # one thing in both hands (2H, bow, crossbow, spell): the left wrist where Gothic holds it
                      # against the right hand
    world = 0.0       # at a mob (a ladder, a workbench): both wrists onto Gothic's
    arm_soft = 0.0

    def arm(self, A, W, out):
        """RenderRig::solveArm: the right arm first, the left one holds on to it."""
        up = out[A['up']]; sh = up[:3, 3].copy()
        toLo = self.local[A['lo']][:3, 3]; toHa = self.local[A['hand']][:3, 3]
        a1 = up[:3, :3] @ toLo
        lo = self.place(A['lo'], W, out)
        a2 = lo[:3, :3] @ toHa
        l1, l2 = np.linalg.norm(a1), np.linalg.norm(a2)
        T = sh + a1 + a2
        if self.hold <= 0 and self.world <= 0:
            out[A['lo']] = lo
            out[A['hand']] = self.place(A['hand'], W, out)
            return
        if A['side'] == 'l' and self.hold > 0:
            R = next(B for B in self.arms if B['side'] == 'r')
            gR, rh = W[R['sh']], out[R['hand']]
            rel = gR[:3, :3].T @ (W[A['sh']][:3, 3] - gR[:3, 3])
            T = T + (rh[:3, 3] + rh[:3, :3] @ self.turn[R['hand']].T @ rel - T) * self.hold
        if self.world > 0:
            T = T + (W[A['sh']][:3, 3] - T) * self.world
        tt = T - sh; dist = np.linalg.norm(tt); u = tt / dist
        d = min(max(dist, abs(l1 - l2) + 1e-6), l1 + l2)
        ds = l1 + l2 - self.arm_soft
        if self.arm_soft > 0 and dist > ds:
            d = ds + self.arm_soft * (1 - math.exp(-(dist - ds) / self.arm_soft))
        along = (l1 * l1 - l2 * l2 + d * d) / (2 * d); h = math.sqrt(max(l1 * l1 - along * along, 0))
        end = sh + u * d
        side = a1 - u * (a1 @ u)                    # the elbow as Gothic's arm bends it
        back = up[:3, :3] @ A['back']; back = back - u * (back @ u)
        side = side + 0.05 * l1 * back / max(np.linalg.norm(back), 1e-9)   # a straight arm: the elbow back
        side /= np.linalg.norm(side)
        elbow = sh + u * along + side * h
        up[:3, :3] = rot_between(a1, elbow - sh) @ up[:3, :3]
        out[A['up']] = up
        lo = self.place(A['lo'], W, out); lo[:3, 3] = elbow
        lo[:3, :3] = rot_between(lo[:3, :3] @ toHa, end - elbow) @ lo[:3, :3]
        out[A['lo']] = lo
        ha = self.place(A['hand'], W, out); ha[:3, 3] = end
        out[A['hand']] = ha

    lift = 1.0
    soft = 0.0
    share = 0.0      # v2 lift: the share of the pelvis' height over the lower of Gothic's ankles
    dy = 0.0

    carry = (0.35, 0.6)   # v3/v4: the hip-ankle line's downward share at which a leg starts / fully carries the pelvis
    sigma = 3.0      # v3: cm over the lowest foot at which a foot's say in the pelvis height falls to 1/e
    swing = (2.0, 10.0)  # v3: cm over the lowest foot where a foot leaves the ground (target from Gothic's shin)

    def knee_lift(self, W):
        """v3: the pelvis height at which the feet on the ground reach their targets with the knee bent as Gothic's
        (our leg lengths, Gothic's knee turn). Each leg gives the vertical shift of the pelvis that makes its hip-ankle
        distance the one of our leg in Gothic's knee angle; the legs are weighted by how low their feet are."""
        pr = self.root_b
        Rp = W[self.source[pr]][:3, :3] @ self.turn[pr]
        pos = (W[self.root_s] @ self.root_local)[:3]
        tgt = {L['side']: self.ankle_of(L, W) for L in self.legs}
        low = min(t[1] for t in tgt.values())
        self.contact = {s: float(np.clip(1 - (t[1] - low - self.swing[0]) / (self.swing[1] - self.swing[0]), 0, 1))
                        for s, t in tgt.items()}
        num = den = 0.0
        for L in self.legs:
            hip = pos + Rp @ (inv(self.Rb[pr]) @ self.Rb[L['thigh']])[:3, 3]
            a1 = W[self.source[L['thigh']]][:3, :3] @ self.turn[L['thigh']] @ self.local[L['calf']][:3, 3]
            a2 = W[self.source[L['calf']]][:3, :3] @ self.turn[L['calf']] @ self.local[L['foot']][:3, 3]
            reach = np.linalg.norm(a1 + a2)
            v = tgt[L['side']] - hip
            disc = v[1] ** 2 - v @ v + reach ** 2
            if disc < 0:
                continue
            r = math.sqrt(disc)
            dy = min(v[1] - r, v[1] + r, key=abs)
            w = math.exp(-(tgt[L['side']][1] - low) / self.sigma)
            down = -v[1] / max(np.linalg.norm(v), 1e-9)              # the leg stands under the hip: it carries
            w *= float(np.clip((down - self.carry[0]) / (self.carry[1] - self.carry[0]), 0, 1))
            num += w * dy; den += w
        return float(np.clip(num / max(den, 1.0), -10.0, 10.0))

    def solve(self, W, legs='engine'):
        out = [None] * len(self.parent)
        self.dy = 0.0
        self.contact = {}
        self.heel = legs == 'v5'
        if legs == 'v5':
            legs = 'v4'
        self.v4 = legs == 'v4'
        self.match = {}
        self.planted = {}
        if self.heel and self.legs:
            self.dy = self.knee_lift_v5(W)
        elif legs == 'v4' and self.legs:
            self.dy = self.knee_lift_v4(W)
        elif legs == 'v3' and self.legs:
            self.dy = self.knee_lift(W)
        elif self.share > 0 and self.legs:
            pel = (W[self.root_s] @ self.root_local)[1]
            low = min((W[L['sf']] @ L['ankle'])[1] for L in self.legs)
            self.dy = self.share * max(0.0, pel - low)
        for i in self.order:
            r = self.role.get(i)
            if r and r[0] in 'CF':
                continue
            if self.arms_ik and any(i in (A['lo'], A['hand']) for A in self.arms):
                continue
            out[i] = self.place(i, W, out)
            if r and r[0] == 'T':
                self.leg(self.legs[r[1]], W, out, legs)
            if self.arms_ik and i in (self.arms[0]['up'], self.arms[1]['up']):
                other = self.arms[1]['up'] if i == self.arms[0]['up'] else self.arms[0]['up']
                if out[other] is None:             # both upper arms first, then the wrists' targets, then the arms
                    continue
                for A in sorted(self.arms, key=lambda A: A['side'] != 'r'):
                    self.arm(A, W, out)
        return np.array(out)


def quat_of(R):
    w = math.sqrt(max(0.0, 1 + np.trace(R))) / 2
    if w > 1e-6:
        v = np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]]) / (4 * w)
    else:                                   # a half turn: the axis from the largest diagonal
        i = int(np.argmax(np.diag(R))); v = np.zeros(3); v[i] = math.sqrt(max(0.0, (R[i, i] + 1) / 2))
        for j in range(3):
            if j != i:
                v[j] = (R[i, j] + R[j, i]) / (4 * v[i])
    return w, v


def rot_axis(ax, ang):
    vx = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
    return np.eye(3) + math.sin(ang) * vx + (1 - math.cos(ang)) * vx @ vx


def twist_of(rig, P, of, against, axis_bone, parent):
    """Signed twist (rad) of `of` against `against` from the bind, about the bind axis parent -> axis_bone."""
    ri, Rb = rig.ri, rig.Rb
    X, Y = ri[against], ri[of]
    D = (P[X][:3, :3].T @ P[Y][:3, :3]) @ (Rb[X][:3, :3].T @ Rb[Y][:3, :3]).T
    a = Rb[ri[axis_bone]][:3, 3] - Rb[ri[parent]][:3, 3]; a = Rb[X][:3, :3].T @ (a / np.linalg.norm(a))
    w, v = quat_of(D)
    if w < 0:
        w, v = -w, -v
    return 2 * math.atan2(v @ a, w)


def twist_bones(rig, P, sides='lr'):
    """World matrices of the twist bones for a solved pose P: {name: 4x4}."""
    out = {}
    ri, Rb = rig.ri, rig.Rb
    for s in sides:
        for name, parent, of, against, axis_bone, t, share in TWISTS:
            name, parent, of, against, axis_bone = (x.format(s=s) for x in (name, parent, of, against, axis_bone))
            B = P[ri[parent]]
            head = B[:3, 3] + t * ((B @ inv(Rb[ri[parent]]) @ Rb[ri[axis_bone]])[:3, 3] - B[:3, 3])
            ax = (B @ inv(Rb[ri[parent]]) @ Rb[ri[axis_bone]])[:3, 3] - B[:3, 3]; ax /= np.linalg.norm(ax)
            M = B.copy(); M[:3, 3] = head
            M[:3, :3] = rot_axis(ax, share * twist_of(rig, P, of, against, axis_bone, parent)) @ B[:3, :3]
            out[name] = M
    return out


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
        keep = rig.lift, rig.share, rig.prev
        rig.lift, rig.share = 1.0, 0.0
        F = rig.solve(W, 'fk'); rig.lift, rig.share, rig.prev = keep
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
