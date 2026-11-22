"""Derive the face profile's leak weights from paired emotional and neutral Audio2Face CSVs."""
import json

import numpy as np

from .cues import DATA, LEAK, LOWER, UPPER
from .face_track import SHAPES

FELT = {'angry': 'anger', 'afraid': 'fear', 'hostile': 'disgust', 'friendly': 'joy', 'pain': 'pain'}
LINES = ('orig_hero', 'orig_biff', 'inworld_xardas')


def leak_table(directory):
    if directory is None:
        directory = DATA/'remaster/hero-head/emotions-1/weights'

    def weights(line, emotion):
        return np.loadtxt(directory/f'{line}.{emotion}.csv', delimiter=',')[:, 1:1+len(SHAPES)]

    out = {}
    for mood, emotion in FELT.items():
        delta = np.mean([weights(line, emotion).mean(0) - weights(line, 'neutral').mean(0) for line in LINES], axis=0)
        for half, shapes in (('upper', UPPER), ('lower', LOWER)):
            values = {k: float(delta[SHAPES.index(k)]) for k in shapes if delta[SHAPES.index(k)] > 0}
            top = max(values.values(), default=0)
            scale = min(LEAK['peak']/top, LEAK['max_scale']) if top > 0 else 0
            out.setdefault(mood, {})[half] = {k: round(x*scale, 3) for k, x in sorted(values.items(), key=lambda kv: -kv[1])
                                               if x*scale >= .03}
    print(json.dumps(out, indent=1))
