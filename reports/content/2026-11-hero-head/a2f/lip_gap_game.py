#!/usr/bin/env python3
"""The lip guard rail of face-shapes.md on the face from the voice in the game (engine-plan.md, step 10).

  lip_gap_game.py <head.glb> <bench face.csv> [<a2f-weights csv>...]

face.csv is what the bench writes for the watched own head: per frame the weights its deformation shows (all layers,
after the rules of speech_face) and vf_w, how far the face from the voice shows. The frames where it shows fully are
played on the head's "face" part at the midline lip contact (the method of shapes/lip_gap.py): the lower lip rides up
over the upper one by more than 2 mm while the jaw is nearly closed (jawOpen < 0.15) - the share of such frames.
The a2f-weights CSVs given after it are measured the same way as they are and with the rules (mouthClose at most
jawOpen, mouthRoll* halved): the same lines outside the game, for comparison with 9.6 % / 35.3 % of face-shapes.md.
"""
import json
import struct
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from face_track import SHAPES  # noqa: E402


def glb(path):
    b = open(path, 'rb').read()
    n = struct.unpack_from('<I', b, 12)[0]
    doc = json.loads(b[20:20+n])
    off = 20+n
    blen = struct.unpack_from('<I', b, off)[0]
    data = b[off+8:off+8+blen]

    def view(i, dtype, count, extra=0):
        v = doc['bufferViews'][i]
        return np.frombuffer(data, dtype, count, v.get('byteOffset', 0)+extra)

    def acc(i):
        a = doc['accessors'][i]
        out = (view(a['bufferView'], np.float32, a['count']*3, a.get('byteOffset', 0)).reshape(-1, 3).copy()
               if 'bufferView' in a else np.zeros((a['count'], 3), np.float32))
        if 'sparse' in a:  # the morph targets: only the vertices they move
            sp = a['sparse']
            kind = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32}[sp['indices']['componentType']]
            idx = view(sp['indices']['bufferView'], kind, sp['count'], sp['indices'].get('byteOffset', 0))
            out[idx] = view(sp['values']['bufferView'], np.float32, sp['count']*3,
                            sp['values'].get('byteOffset', 0)).reshape(-1, 3)
        return out
    parts = {}
    for m in doc['meshes']:
        p = m['primitives'][0]
        names = m.get('extras', {}).get('targetNames', [])
        parts[m['name']] = (acc(p['attributes']['POSITION']),
                            {nm: acc(t['POSITION']) for nm, t in zip(names, p.get('targets', []))})
    return parts


def contact(parts):
    V, sk = parts['face']
    unit = lambda v: v/np.linalg.norm(v)  # noqa: E731
    jaw = np.linalg.norm(sk['jawOpen'], axis=1)
    cl = V[np.argsort(-np.linalg.norm(sk['mouthSmileLeft'], axis=1))[:3]].mean(0)
    cr = V[np.argsort(-np.linalg.norm(sk['mouthSmileRight'], axis=1))[:3]].mean(0)
    mc, left = (cl+cr)/2, unit(cl-cr)
    eyes = (parts['eye_L'][0].mean(0)+parts['eye_R'][0].mean(0))/2
    down = unit(mc-eyes)
    down = unit(down-left*(down @ left))
    fwd = unit(np.cross(down, left))
    pk = sk['mouthPucker'][np.linalg.norm(sk['mouthPucker'], axis=1) > 0.004].mean(0)
    fwd = fwd if pk @ fwd > 0 else -fwd
    pc = V[np.argsort(-np.linalg.norm(sk['mouthPucker'], axis=1))[:5]].mean(0)
    mid = (np.abs((V-pc) @ left) < 0.002) & (np.linalg.norm(V-pc, axis=1) < 0.015)
    front = mid & ((V-pc) @ fwd > -0.006)
    lo = np.nonzero(front & (jaw > 0.4*jaw[front].max()))[0]
    up = np.nonzero(front & (jaw < 0.2*jaw[front].max()))[0]
    iu, il = up[np.argmax((V[up]-pc) @ down)], lo[np.argmin((V[lo]-pc) @ down)]
    return V, sk, iu, il, down


def gaps(model, names, W):
    V, sk, iu, il, down = model
    D = np.stack([sk[n][[iu, il]] if n in sk else np.zeros((2, 3), np.float32) for n in names])
    P = V[[iu, il]][None]+np.einsum('fk,kpc->fpc', W, D)
    return (P[:, 1]-P[:, 0]) @ down


def report(label, gap, jaw):
    c = jaw < 0.15
    over = (c & (gap < -0.002)).sum()/max(c.sum(), 1)
    print('%-34s frames %5d, near-closed %5d: lower lip over the upper > 2 mm %5.1f %%, worst %5.1f mm'
          % (label, len(gap), c.sum(), 100*over, (gap[c].min() if c.any() else 0)*1000))


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    model = contact(glb(argv[0]))
    print('contact: upper %d lower %d, rest gap %.2f mm' % (model[2], model[3], (model[0][model[3]]-model[0][model[2]]) @ model[4]*1000))
    head = open(argv[1]).readline().strip().split(',')
    rows = (np.loadtxt(argv[1], delimiter=',', skiprows=1, usecols=range(3, len(head)), ndmin=2)
            if len(head) > 5 else np.zeros((0, 2)))
    vf = rows[:, 0]
    names = head[5:]
    W = rows[:, 2:]
    on = vf >= 0.99
    if on.any():
        report('game, face from the voice', gaps(model, names, W[on]), W[on][:, names.index('jawOpen')])
    else:
        print('game: no frame with the face from the voice')
    for csv in argv[2:]:
        raw = np.loadtxt(csv, delimiter=',')[:, 1:1+len(SHAPES)]
        report(csv.rsplit('/', 1)[-1]+' as is', gaps(model, SHAPES, raw), raw[:, SHAPES.index('jawOpen')])
        fix = raw.copy()
        j, c = SHAPES.index('jawOpen'), SHAPES.index('mouthClose')
        fix[:, c] = np.minimum(fix[:, c], fix[:, j])
        for n in ('mouthRollLower', 'mouthRollUpper'):
            fix[:, SHAPES.index(n)] *= 0.5
        report(csv.rsplit('/', 1)[-1]+' with the rules', gaps(model, SHAPES, fix), fix[:, j])


if __name__ == '__main__':
    main(sys.argv[1:])
