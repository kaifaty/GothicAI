# Facial gestures and the leak of a felt emotion, added offline to Audio2Face weight tracks (face-cues.md, step 1).
# Writes the weight CSVs for the two review clips and a plan (apex times, answer key) for cues.sh and sheet.py.
#   python cues.py <out dir> [--seed N]
# <out dir>/a/<cell>.csv     clip A: line orig_hero, plain and each gesture twice
# <out dir>/b/segNN.csv      clip B: ten 4.5 s halves of the lines, calm, one ordinary gesture each, 4-5 with a leak
# <out dir>/plan.json, <out dir>/answers.txt
# CSV layout as a2f-weights: timestamp in 16 kHz samples, 52 ARKit skin weights (order SKIN), 16 tongue weights.
import json, sys, wave
from pathlib import Path
import numpy as np

SKIN = ("eyeBlinkLeft eyeLookDownLeft eyeLookInLeft eyeLookOutLeft eyeLookUpLeft eyeSquintLeft eyeWideLeft "
        "eyeBlinkRight eyeLookDownRight eyeLookInRight eyeLookOutRight eyeLookUpRight eyeSquintRight eyeWideRight "
        "jawForward jawLeft jawRight jawOpen mouthClose mouthFunnel mouthPucker mouthLeft mouthRight mouthSmileLeft "
        "mouthSmileRight mouthFrownLeft mouthFrownRight mouthDimpleLeft mouthDimpleRight mouthStretchLeft "
        "mouthStretchRight mouthRollLower mouthRollUpper mouthShrugLower mouthShrugUpper mouthPressLeft "
        "mouthPressRight mouthLowerDownLeft mouthLowerDownRight mouthUpperUpLeft mouthUpperUpRight browDownLeft "
        "browDownRight browInnerUp browOuterUpLeft browOuterUpRight cheekPuff cheekSquintLeft cheekSquintRight "
        "noseSneerLeft noseSneerRight tongueOut").split()
J = {k: i for i, k in enumerate(SKIN)}
SR, FPS = 16000, 30
DATA = Path.home()/'.local/share/gothic-ai'
NEUTRAL = DATA/'remaster/hero-head/face-shapes-2/weights/{line}.v3.csv'
FELT = [DATA/'remaster/hero-head/emotions-1/weights/{line}.{emo}.csv',
        DATA/'remaster/hero-head/cues-1/weights/{line}.{emo}.csv']     # the last: runs made for this probe
WAV = DATA/'face-sources/clips/{line}.wav'
LINES = 'inworld_hero inworld_xardas orig_hero orig_sylvio orig_biff player_mic'.split()

def LR(names, v):  # both sides of each shape stem: LR('browDown', .5) -> browDownLeft/Right
    return {n+s: v for n in names.split() for s in ('Left', 'Right')}

# The dictionary. Each gesture: parts (weights at apex, delay, onset, hold, offset in s), the zone it belongs to
# ('mouth' gestures go only into pauses of the voice), and for one-sided ones the side as written (mirrored on demand).
G = {
    'flash':   dict(ru='вспышка бровей', zone='upper', parts=[
        ({'browInnerUp': .55, **LR('browOuterUp', .70), **LR('eyeWide', .25)}, 0, .10, .08, .16)]),
    'doubt':   dict(ru='сомнение', zone='upper', side='L', parts=[
        ({'browOuterUpLeft': .80, 'browInnerUp': .15, 'browDownRight': .65, 'eyeSquintRight': .25}, 0, .25, .55, .30)]),
    'smirk':   dict(ru='ухмылка', zone='mouth', side='L', parts=[
        ({'mouthSmileLeft': .75, 'mouthDimpleLeft': .55, 'cheekSquintLeft': .20, 'mouthPressRight': .15},
         0, .20, .40, .30)]),
    'press':   dict(ru='поджатые губы', zone='mouth', parts=[
        ({**LR('mouthPress', .70), 'mouthRollLower': .20, 'mouthRollUpper': .15, 'mouthShrugLower': .12},
         0, .15, .40, .20)]),
    'disgust': dict(ru='брезгливость', zone='upper', parts=[
        ({**LR('noseSneer', .60), **LR('mouthUpperUp', .06), **LR('cheekSquint', .25), **LR('browDown', .20)},
         0, .15, .30, .20)]),
    'squint':  dict(ru='прищур подозрения', zone='upper', parts=[
        ({**LR('eyeSquint', .75), **LR('eyeBlink', .12), **LR('browDown', .20)}, 0, .25, .80, .35)]),
    'frown':   dict(ru='хмурится', zone='upper', parts=[
        ({**LR('browDown', .75), **LR('eyeSquint', .15)}, 0, .30, .90, .40)]),
    'chin':    dict(ru='подбородок', zone='mouth', parts=[
        ({'mouthShrugLower': .70, 'mouthShrugUpper': .15, **LR('mouthPress', .15), **LR('mouthFrown', .25)},
         0, .15, .40, .25)]),
    'wince':   dict(ru='поморщился', zone='upper', parts=[
        ({**LR('cheekSquint', .65), **LR('eyeSquint', .65), **LR('noseSneer', .45), **LR('browDown', .30),
          **LR('eyeBlink', .25)}, 0, .08, .10, .20)]),
    'glance':  dict(ru='взгляд в сторону', zone='upper', side='R', parts=[
        ({'eyeLookOutRight': .80, 'eyeLookInLeft': .80}, 0, .08, .60, .12)]),
    'roll':    dict(ru='закатил глаза', zone='upper', parts=[      # an arc: from the right, up, to the left, back
        ({**LR('eyeLookUp', .90), **LR('eyeBlink', .20), 'browInnerUp': .15}, .05, .25, .30, .30),
        ({'eyeLookOutRight': .40, 'eyeLookInLeft': .40}, 0, .12, .05, .20),
        ({'eyeLookOutLeft': .40, 'eyeLookInRight': .40}, .45, .20, .10, .25)]),
}
ORDER = 'flash doubt smirk press disgust squint frown chin wince glance roll'.split()
# the leak: felt minus neutral of the same line, one half of the face only
UPPER = [k for k in SKIN if k.startswith(('brow', 'eyeSquint', 'eyeWide', 'eyeBlink'))]
LOWER = [k for k in SKIN if k.startswith(('mouthSmile', 'mouthFrown', 'mouthDimple', 'mouthStretch', 'mouthPress',
                                          'mouthShrug', 'mouthUpperUp', 'cheek', 'noseSneer'))]
# 0.40 s in all, onset 0.12 s; the felt-minus-neutral shape is scaled so its largest weight is `peak` (an even
# strength of the sign across feelings and halves, face-cues.md), but never above `max_scale` times the felt run
LEAK = dict(on=.12, hold=.12, off=.16, peak=.45, max_scale=1.3)
BLINK = dict(on=.05, hold=.03, off=.08)             # ~0.15 s

def mirror(k):
    return k.replace('Left', '#').replace('Right', 'Left').replace('#', 'Right')

def load(path):
    d = np.loadtxt(path, delimiter=',')
    return d[:, 0], d[:, 1:1+len(SKIN)].copy(), d[:, 1+len(SKIN):]

def save(path, t, W, rest):
    path.parent.mkdir(parents=True, exist_ok=True)
    np.savetxt(path, np.column_stack([t, W, rest]), delimiter=',', fmt=['%d']+['%.4f']*(W.shape[1]+rest.shape[1]))

def ease(x):
    x = np.clip(x, 0, 1)
    return x*x*(3-2*x)

def env(ts, t0, on, hold, off):
    return ease((ts-t0)/on)*(1-ease((ts-t0-on-hold)/off))

def parts(g, hold=None):
    """The gesture's parts, the first one's hold replaced when given (a mouth gesture squeezed into a pause)."""
    p = G[g]['parts']
    return p if hold is None else [p[0][:3]+(hold,)+p[0][4:]]+p[1:]

def span(g, hold=None):
    return max(d+on+h+off for _, d, on, h, off in parts(g, hold))

def apex(g, t0, hold=None):
    _, d, on, h, _ = parts(g, hold)[0]
    return t0+d+on+h/2

class Track:
    """One line's weights in seconds (60 Hz rows), with the gesture layer kept apart until written."""
    def __init__(self, line, felt=None):
        self.line = line
        self.t, self.base, self.rest = load(str(NEUTRAL).format(line=line))
        self.ts = self.t/SR
        self.add = np.zeros_like(self.base)
        self.windows = []   # (kind, start, end, shapes)

    def gesture(self, g, t0, side=None, hold=None):
        flip = side is not None and G[g].get('side') not in (None, side)
        shapes = set()
        for w, d, on, h, off in parts(g, hold):
            e = env(self.ts, t0+d, on, h, off)
            for k, v in w.items():
                k = mirror(k) if flip else k
                self.add[:, J[k]] += v*e
                shapes.add(k)
        self.windows.append(('gesture', t0, t0+span(g, hold), shapes))

    def leak(self, emo, half, t0):
        felt = next(p for p in (str(f).format(line=self.line, emo=emo) for f in FELT) if Path(p).exists())
        _, F, _ = load(felt)
        shapes = UPPER if half == 'upper' else LOWER
        win = (self.ts >= t0-.25) & (self.ts <= t0+.65)
        delta = (F-self.base)[win].mean(0)
        scale = min(LEAK['peak']/np.abs(delta[[J[k] for k in shapes]]).max(), LEAK['max_scale'])
        e = env(self.ts, t0, LEAK['on'], LEAK['hold'], LEAK['off'])
        for k in shapes:
            self.add[:, J[k]] += scale*delta[J[k]]*e
        self.windows.append(('leak', t0, t0+LEAK['on']+LEAK['hold']+LEAK['off'], set(shapes)))
        return {k: round(float(scale*delta[J[k]]), 3) for k in shapes if abs(scale*delta[J[k]]) >= .03}

    def blink(self, t0):
        e = env(self.ts, t0, BLINK['on'], BLINK['hold'], BLINK['off'])
        for k in ('eyeBlinkLeft', 'eyeBlinkRight'):
            self.add[:, J[k]] += e
        self.windows.append(('blink', t0, t0+sum(BLINK.values()), {'eyeBlinkLeft', 'eyeBlinkRight'}))

    def frames(self, a=0.0, b=None):
        """Rows the renderer shows at 30 fps for the cut [a, b) (same rule as render_a2f.py)."""
        rows = np.flatnonzero((self.ts >= a-1e-9) & (self.ts < (b if b is not None else 1e9)-1e-9))
        ts = self.ts[rows]-self.ts[rows[0]]
        n = int(np.floor(ts[-1]*FPS))+1
        return rows[np.clip(np.searchsorted(ts, np.arange(n)/FPS+1e-6, side='right')-1, 0, len(rows)-1)]

    def clamps(self, a=0.0, b=None):
        """Rendered frames inside gesture/leak windows where some added-to weight would pass 1 (or go below 0).
        A blink closing the lid to 1 by design is not counted."""
        raw = self.base+self.add
        hi = lo = 0
        for r in self.frames(a, b):
            s = self.ts[r]
            act = [w for w in self.windows if w[0] != 'blink' and w[1] <= s <= w[2]]
            if not act:
                continue
            blinking = any(w[0] == 'blink' and w[1] <= s <= w[2] for w in self.windows)
            ks = [J[k] for w in act for k in w[3] if not (blinking and k.startswith('eyeBlink'))]
            hi += bool((raw[r, ks] >= 1.0).any())
            lo += bool((raw[r, ks] < 0).any())
        return hi, lo

    def write(self, path, a=0.0, b=None):
        rows = np.flatnonzero((self.ts >= a-1e-9) & (self.ts < (b if b is not None else 1e9)-1e-9))
        W = np.clip(self.base+self.add, 0, 1)
        save(path, self.t[rows]-self.t[rows[0]], W[rows], self.rest[rows])

def pauses(line, min_len=.25, below_db=30):
    """Pauses of the voice: >= min_len s with the 25 ms loudness below its 95th percentile minus below_db."""
    w = wave.open(str(WAV).format(line=line))
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16)/32768.0
    hop = w.getframerate()//100
    db = 10*np.log10(np.convolve(x**2, np.ones(400)/400, 'same')[::hop]+1e-12)
    q = np.r_[False, db < np.percentile(db, 95)-below_db, False]
    edges = np.flatnonzero(np.diff(q.astype(int)))
    return [(float(a*.01), float(b*.01)) for a, b in zip(edges[::2], edges[1::2]) if (b-a)*.01 >= min_len]

def fit_mouth(g, pause, lo=-.10, hi=.15):
    """(start, hold) of a mouth gesture lying within the pause widened by lo/hi s, the hold shortened if needed;
    None when even a 0.1 s hold does not fit."""
    a, b = pause[0]+lo, pause[1]+hi
    _, d, on, h, off = G[g]['parts'][0]
    room = (b-a)-d-on-off
    return None if room < .1 else (a, min(h, room))

def dictionary(out):
    line = 'orig_hero'
    ps = sorted(sorted(pauses(line), key=lambda p: p[0]-p[1])[:2])
    cells, plain = [], Track(line)
    plain.write(out/'a'/'plain.csv')
    cells.append(dict(key='plain', ru='без жестов', apex=[]))
    for g in ORDER:
        tr, starts, apexes, holds = Track(line), [], [], []
        for p in ps:
            t0, hold = fit_mouth(g, p) if G[g]['zone'] == 'mouth' else (p[0]-.10, None)
            tr.gesture(g, t0, hold=hold)
            starts.append(round(t0, 3)); apexes.append(round(apex(g, t0, hold), 3)); holds.append(hold)
        tr.write(out/'a'/f'{g}.csv')
        hi, lo = tr.clamps()
        cells.append(dict(key=g, ru=G[g]['ru'], start=starts, apex=apexes, hold=holds, clamp_hi=hi, clamp_lo=lo))
    return dict(line=line, pauses=ps, cells=cells)

def blind(out, seed):
    rng = np.random.default_rng(seed)
    SEG = 4.5
    halves = [(l, h) for l in LINES for h in (0, 1)]
    pick = [halves[i] for i in rng.permutation(len(halves))[:10]]
    while True:
        n_lies = int(rng.choice([4, 5]))
        lies = sorted(rng.choice(10, n_lies, replace=False).tolist())
        felt = [str(rng.choice(['fear', 'anger', 'disgust'])) for _ in lies]
        face = [str(rng.choice(['upper', 'lower'])) for _ in lies]
        if len(set(felt)) >= 2 and len(set(face)) == 2:
            break
    ordinary = [g for g in ORDER if g not in ('wince', 'disgust')]   # the two that look like a felt emotion
    deck, segs = [], []
    for i, (line, h) in enumerate(pick):
        a = h*SEG
        tr = Track(line)
        ps = [(max(p[0], a)-a, min(p[1], a+SEG)-a) for p in pauses(line)
              if min(p[1], a+SEG)-max(p[0], a) >= .25 and p[0] >= a+.3 and p[1] <= a+SEG-.2]
        fits = {g: [f for f in (fit_mouth(g, p) for p in ps) if f] for g in ordinary if G[g]['zone'] == 'mouth'}
        if not deck:   # each ordinary gesture once before any repeats
            deck = [ordinary[k] for k in rng.permutation(len(ordinary))]
        g = next(g for g in deck if G[g]['zone'] != 'mouth' or fits[g])
        deck.remove(g)
        side = str(rng.choice(['L', 'R'])) if G[g].get('side') else None
        if G[g]['zone'] == 'mouth':
            g0, hold = fits[g][int(rng.integers(len(fits[g])))]
        else:
            g0, hold = float(rng.uniform(.4, SEG-span(g)-.4)), None
        tr.gesture(g, a+g0, side, hold)
        seg = dict(n=i+1, line=line, half=['first', 'second'][h], gesture=g, gesture_ru=G[g]['ru'], side=side,
                   gesture_start=round(g0, 3), gesture_apex=round(apex(g, g0, hold), 3))
        busy = [(g0-.4, g0+span(g, hold)+.4)]
        if i in lies:
            j = lies.index(i)
            dur = LEAK['on']+LEAK['hold']+LEAK['off']
            while True:
                l0 = float(rng.uniform(.5, SEG-dur-.5))
                if all(l0+dur < b0 or l0 > b1 for b0, b1 in busy):
                    break
            seg['leak'] = dict(felt=felt[j], face=face[j], start=round(l0, 3), apex=round(l0+LEAK['on']+LEAK['hold']/2, 3),
                               weights=tr.leak(felt[j], face[j], a+l0))
            busy.append((l0-.15, l0+dur+.15))
        n_blinks = int(rng.integers(0, 4)) + (2 if i in lies else 0)   # ordinary 0-3, a liar 2 more
        blinks = []
        for _ in range(5000):
            if len(blinks) == n_blinks:
                break
            b0 = float(rng.uniform(.3, SEG-.4))
            if all(abs(b0-x) > .5 for x in blinks) and all(b0+.16 < x0 or b0 > x1 for x0, x1 in busy[1:]):
                blinks.append(b0)
        for b0 in sorted(blinks):
            tr.blink(a+b0)
        seg['blinks'] = [round(b, 2) for b in sorted(blinks)]
        seg['clamp_hi'], seg['clamp_lo'] = tr.clamps(a, a+SEG)
        tr.write(out/'b'/f'seg{i+1:02d}.csv', a, a+SEG)
        segs.append(seg)
    return dict(seed=seed, seg_len=SEG, leak=LEAK, segments=segs)

def answers(b):
    out = [f'Clip B answer key (seed {b["seed"]}). Times are from the start of the segment, s.', '']
    for s in b['segments']:
        lk = s.get('leak')
        what = (f"LEAK  {lk['felt']:7s} {lk['face']} face, {lk['start']:.2f}-{lk['start']+.40:.2f}" if lk
                else 'none')
        out.append(f"{s['n']:2d}  {what:40s} | {s['line']} {s['half']} half; ordinary gesture "
                   f"«{s['gesture_ru']}»{' '+s['side'] if s['side'] else ''} at {s['gesture_start']:.2f}; "
                   f"blinks {', '.join(f'{x:.2f}' for x in s['blinks'])}")
    return '\n'.join(out)+'\n'

if __name__ == '__main__':
    out = Path(sys.argv[1])
    seed = int(sys.argv[sys.argv.index('--seed')+1]) if '--seed' in sys.argv else 2026
    plan = dict(gestures={g: dict(G[g], span=round(span(g), 2)) for g in ORDER},
                a=dictionary(out), b=None)
    plan['b'] = blind(out, seed)
    (out/'plan.json').write_text(json.dumps(plan, ensure_ascii=False, indent=1))
    (out/'answers.txt').write_text(answers(plan['b']))
    a = plan['a']['cells'][1:]
    print('A pauses', [(round(float(x), 2), round(float(y), 2)) for x, y in plan['a']['pauses']])
    for c in a:
        print(f"A {c['key']:8s} start {c['start']} apex {c['apex']} clamp>=1 {c['clamp_hi']} <0 {c['clamp_lo']}")
    for s in plan['b']['segments']:
        print(f"B {s['n']:2d} {s['line']:15s} {s['half']:6s} {s['gesture']:8s} leak {'yes' if 'leak' in s else '-'} "
              f"blinks {len(s['blinks'])} clamp>=1 {s['clamp_hi']} <0 {s['clamp_lo']}")
