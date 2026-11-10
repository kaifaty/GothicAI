# The leak table for the game (face-cues.md, step 4): per felt emotion and half of the face, the shape weights
# at the apex of a leak. Felt minus neutral Audio2Face runs of the probe lines, averaged over the whole line,
# restricted to the half's expression shapes (UPPER/LOWER of cues.py), positive part only, scaled so the largest
# weight is LEAK peak 0.45 (never above 1.3x the averaged delta), weights under 0.03 dropped.
#   python leak_table.py [weights dir]   -> JSON on stdout
import json, sys
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
UPPER = [k for k in SKIN if k.startswith(('brow', 'eyeSquint', 'eyeWide', 'eyeBlink'))]
LOWER = [k for k in SKIN if k.startswith(('mouthSmile', 'mouthFrown', 'mouthDimple', 'mouthStretch', 'mouthPress',
                                          'mouthShrug', 'mouthUpperUp', 'cheek', 'noseSneer'))]
FELT = {'angry': 'anger', 'afraid': 'fear', 'hostile': 'disgust', 'friendly': 'joy', 'pain': 'pain'}
LINES = ('orig_hero', 'orig_biff', 'inworld_xardas')
PEAK, MAX_SCALE, FLOOR = .45, 1.3, .03
d = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home()/'.local/share/gothic-ai/remaster/hero-head/emotions-1/weights'
w = lambda line, emo: np.loadtxt(d/f'{line}.{emo}.csv', delimiter=',')[:, 1:1+len(SKIN)]
out = {}
for mood, emo in FELT.items():
    delta = np.mean([w(l, emo).mean(0) - w(l, 'neutral').mean(0) for l in LINES], axis=0)
    for half, shapes in (('upper', UPPER), ('lower', LOWER)):
        v = {k: float(delta[SKIN.index(k)]) for k in shapes if delta[SKIN.index(k)] > 0}
        top = max(v.values(), default=0)
        s = min(PEAK/top, MAX_SCALE) if top > 0 else 0
        out.setdefault(mood, {})[half] = {k: round(x*s, 3) for k, x in sorted(v.items(), key=lambda kv: -kv[1]) if x*s >= FLOOR}
print(json.dumps(out, indent=1))
