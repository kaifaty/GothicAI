# Guard rail of face-shapes.md: how often the lower lip rides up over the upper one while the jaw is nearly closed,
# and whether the lips close where the network closed them. Plays Audio2Face weights on heads at the midline lip
# contact and measures the gap along "down".
#   python lip_gap.py <weights dir> <MPFB head.glb> [<label>=<head.glb>:<weights>] ...   (in ~/.local/share/gothic-ai/mpfb/venv)
# <weights dir>/<line>.<weights>.csv are a2f-weights CSVs (v3: as solved for Mark, v3fix: with the lip rules). Rows:
# the MPFB head with v3fix (the rules of speech_face) and v3, then every given head with its weights; all heads share
# the MPFB head's mesh (the contact is found on it, a2f/lip_gap_game.py's way).
# The second measure's reference is Mark's own face (bs_skin_Mark.npz) with v3: where its lips close (gap < 1 mm,
# scaled to the head, from its rest), a head must close them too; a set that never rides over may just never close.
import glob
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'a2f'))
sys.path.insert(0, HERE)
from lip_gap_game import contact, glb  # noqa: E402
from face_track import SHAPES  # noqa: E402
import transfer as X  # noqa: E402


def lines(wt, w):
    return sorted(glob.glob(os.path.join(wt, '*.%s.csv' % w)))


def head_gaps(model, sk, files):
    V, _, iu, il, down = model
    D = np.stack([sk[n][[iu, il]] if n in sk else np.zeros((2, 3)) for n in SHAPES])
    G, J = [], []
    for f in files:
        W = np.loadtxt(f, delimiter=',')[:, 1:1+len(SHAPES)]
        P = V[[iu, il]][None]+np.einsum('fk,kpc->fpc', W, D)
        G.append((P[:, 1]-P[:, 0]) @ down)
        J.append(W[:, SHAPES.index('jawOpen')])
    return np.concatenate(G), np.concatenate(J)


def mark_reference(scale_to):
    """The lip gap of Mark's own face: its midline contact (transfer.landmarks), in the head's units by the ratio of
    the eyes-to-mouth heights. Returns a function of the weights."""
    mk = X.load_mark()
    fr = X.frame_and_landmarks(mk)
    iu, il = fr['lm_v']['lipContactUpper'], fr['lm_v']['lipContactLower']
    k = scale_to/fr['h']
    D = np.stack([mk['S'][n][[iu, il]] if n in mk['S'] else np.zeros((2, 3)) for n in SHAPES])
    P0 = mk['V'][[iu, il]]

    rest = (P0[1]-P0[0]) @ fr['down']*k
    print('Mark\'s contact: rest gap %.2f mm (closed lips; the two vertices lie on neighbouring rows), measured from it' % (rest*1000))

    def gap(W):
        P = P0[None]+np.einsum('fk,kpc->fpc', W, D)
        return (P[:, 1]-P[:, 0]) @ fr['down']*k-rest
    return gap


def main(argv):
    wt, mpfb = os.path.expanduser(argv[0]), os.path.expanduser(argv[1])
    parts = glb(mpfb)
    model = contact(parts)
    V, sk, iu, il, down = model
    eyes = (parts['eye_L'][0].mean(0)+parts['eye_R'][0].mean(0))/2
    h = abs((V[iu]-eyes) @ down)
    print('contact: upper %d lower %d, rest gap %.2f mm; eyes to mouth %.0f mm' % (iu, il, ((V[il]-V[iu]) @ down)*1000,
                                                                                h*1000))
    ref_gap = mark_reference(h)
    ref = np.concatenate([ref_gap(np.loadtxt(f, delimiter=',')[:, 1:1+len(SHAPES)]) for f in lines(wt, 'v3')])
    rows = [('MPFB с поправкой', sk, 'v3fix'), ('MPFB без поправки', sk, 'v3')]
    for a in argv[2:]:
        label, rest = a.split('=', 1)
        path, w = rest.rsplit(':', 1)
        rows.append((label, glb(os.path.expanduser(path))['face'][1], w))
    print('reference: Mark\'s own face with v3, %d frames, lips shut (< 1 mm) in %d' % (len(ref), (ref < 0.001).sum()))
    print('| Набор и веса | Губа за верхней > 2 мм, доля почти закрытых кадров | Худший кадр, мм | Открыт > 1 мм, где Mark '
          'сомкнут | Открыт > 3 мм, где Mark сомкнут | Средняя разница щели с Mark, мм |')
    for label, shapes, w in rows:
        files = lines(wt, w)
        if len(files) != len(lines(wt, 'v3')):
            print('%s: %d CSV of %s, %d of v3 - skipped' % (label, len(files), w, len(lines(wt, 'v3'))))
            continue
        gap, jaw = head_gaps(model, shapes, files)
        c = jaw < 0.15
        over = (c & (gap < -0.002)).sum()/c.sum()
        shut = ref < 0.001
        g = gap-(V[il]-V[iu]) @ down                  # from the head's rest, like the reference
        print('| %s | %.1f %% | %.1f | %.1f %% | %.1f %% | %.1f |' % (
            label, 100*over, gap[c].min()*1000, 100*(shut & (g > 0.001)).sum()/shut.sum(),
            100*(shut & (g > 0.003)).sum()/shut.sum(), np.abs(g-ref).mean()*1000))


if __name__ == '__main__':
    main(sys.argv[1:])
