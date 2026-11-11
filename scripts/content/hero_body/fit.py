"""Fit the Tripo hero onto the Gothic HUMANS skeleton.

Frames: T = glTF frame of the GLB in cm (floor y=0, faces +Z, character's left at +X).
W = T * S (work frame, right-handed, cm). Gothic G = (-W.x, W.y - FLOOR, W.z + TZ).
"""
import json
import numpy as np

S = 1.9            # 98.04 cm model -> 186.3 cm, matches shoulder, neck and head heights of HUMANS.MDH
FLOOR = 99.95      # HUMANS.MDH root translation: Gothic y of the floor is -FLOOR
F = np.diag([-1.0, 1.0, 1.0])

def load_skeleton(path):
    d = json.load(open(path))['data']
    nodes = d['nodes']
    names = [n['name'] for n in nodes]
    parent = [n['parent_index'] for n in nodes]
    local = [np.array(n['transform'], dtype=np.float64).reshape(4, 4).T for n in nodes]
    glob = []
    for i, m in enumerate(local):
        glob.append(m.copy() if parent[i] < 0 else glob[parent[i]] @ m)
    return names, parent, local, glob

def landmarks_work(path):
    L = json.load(open(path))
    return {k: np.array(v) * S for k, v in L.items()}

def tz_of(LW):
    return 0.6 - LW['pelvis'][2]

def w2g(p, tz):
    p = np.asarray(p, dtype=np.float64)
    return np.stack([-p[..., 0], p[..., 1] - FLOOR, p[..., 2] + tz], -1)

def g2w(p, tz):
    p = np.asarray(p, dtype=np.float64)
    return np.stack([-p[..., 0], p[..., 1] + FLOOR, p[..., 2] - tz], -1)

def rot_between(a, b):
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(a @ b)
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K * (1 / (1 + c))

# node -> (child node, landmark from, landmark to); side letter substituted
AIM = [('BIP01 {S} UPPERARM', 'BIP01 {S} FOREARM', 'shoulder_{s}', 'elbow_{s}'),
       ('BIP01 {S} FOREARM', 'BIP01 {S} HAND', 'elbow_{s}', 'wrist_{s}'),
       ('BIP01 {S} HAND', 'BIP01 {S} FINGER0', 'wrist_{s}', 'fingertip_{s}'),
       ('BIP01 {S} THIGH', 'BIP01 {S} CALF', 'hip_{s}', 'knee_{s}'),
       ('BIP01 {S} CALF', 'BIP01 {S} FOOT', 'knee_{s}', 'ankle_{s}')]

def posed_skeleton(skel, LW):
    names, parent, local, glob = skel
    tz = tz_of(LW)
    LG = {k: w2g(v, tz) for k, v in LW.items()}
    aim = {}
    for side in 'LR':
        for node, child, a, b in AIM:
            aim[node.format(S=side)] = (names.index(child.format(S=side)), LG[a.format(s=side)], LG[b.format(s=side)])
    posed = []
    for i in range(len(names)):
        g = local[i].copy() if parent[i] < 0 else posed[parent[i]] @ local[i]
        if names[i] in aim:
            ci, a, b = aim[names[i]]
            cur = (g @ local[ci])[:3, 3] - g[:3, 3]
            R = rot_between(cur, b - a)
            g[:3, :3] = R @ g[:3, :3]
        posed.append(g)
    return posed

def control_points(skel, posed, LW):
    """Tripo points (W) and where they must go (W)."""
    names = skel[0]
    tz = tz_of(LW)
    J = lambda n: g2w(posed[names.index(n)][:3, 3], tz)
    src, dst = [], []
    def add(p, q): src.append(np.asarray(p, float)); dst.append(np.asarray(q, float))
    add(LW['pelvis'], LW['pelvis'])
    for s, S_ in (('L', 'L'), ('R', 'R')):
        add(LW[f'hip_{s}'], J(f'BIP01 {S_} THIGH'))
        add(LW[f'knee_{s}'], J(f'BIP01 {S_} CALF'))
        for k in ('ankle', 'toe', 'heel'):          # the boot keeps its shape and stands on the floor
            add(LW[f'{k}_{s}'], LW[f'{k}_{s}'])
        add(LW[f'shoulder_{s}'], J(f'BIP01 {S_} UPPERARM'))
        add(LW[f'elbow_{s}'], J(f'BIP01 {S_} FOREARM'))
        add(LW[f'wrist_{s}'], J(f'BIP01 {S_} HAND'))
        d = J(f'BIP01 {S_} HAND') - LW[f'wrist_{s}']   # the hand moves rigidly with the wrist
        add(LW[f'fingertip_{s}'], LW[f'fingertip_{s}'] + d)
    add(LW['neck'], J('BIP01 NECK'))
    add(LW['head'], J('BIP01 HEAD'))
    add(LW['top'], LW['top'] + (J('BIP01 HEAD') - LW['head']))
    return np.array(src), np.array(dst)

class TPS:
    """3D polyharmonic spline (phi = r) with an affine part; maps W -> W displacement."""
    def __init__(self, src, dst, reg=1e-3):
        n = len(src)
        K = np.linalg.norm(src[:, None] - src[None], axis=2) + reg * np.eye(n)
        P = np.c_[np.ones(n), src]
        A = np.zeros((n + 4, n + 4)); A[:n, :n] = K; A[:n, n:] = P; A[n:, :n] = P.T
        b = np.zeros((n + 4, 3)); b[:n] = dst - src
        self.src = src; self.coef = np.linalg.solve(A, b)
    def __call__(self, p, chunk=200000):
        out = np.empty_like(p)
        n = len(self.src)
        for i in range(0, len(p), chunk):
            q = p[i:i + chunk]
            K = np.linalg.norm(q[:, None] - self.src[None], axis=2)
            out[i:i + chunk] = q + K @ self.coef[:n] + np.c_[np.ones(len(q)), q] @ self.coef[n:]
        return out
