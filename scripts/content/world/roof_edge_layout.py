#!/usr/bin/env python3
"""Shingle butts of NW_CITY_ROOF_01 (our roofs-8 bake), for the real edge of the roofs in the world mesh.

roofs-8 is shingle_bake.py's model laid out with --seed 1 (city-roofs, attempt 4). This rebuilds that layout
and writes, for every shingle, its course, its butt thickness and its butt edge: the lowest point of its
outline at nine places across it, in metres of the texture square (x along U, y up the slope, V = 1 - y /
height). roof_edge.py puts the butts at the eaves and the course steps at the verges where the texture
paints them:

    uv run --no-project --with numpy --with shapely python scripts/content/world/roof_edge_layout.py \
        data/content/world/roof01-shingles.json
"""
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import shingle_bake  # noqa: E402

SEED = 1           # roofs-8: shingle_bake.py layout --seed 1
SAMPLES = 9        # points of the butt edge across a shingle


def butt_edge(outline, samples=SAMPLES):
    """x of the shingle's sides at the butt and the butt's y at `samples` places between them."""
    pts = np.asarray(outline)
    low = pts[:, 1] < pts[:, 1].min() + .08          # the butt end of the outline
    x0, x1 = pts[low, 0].min(), pts[low, 0].max()
    xs = np.linspace(x0, x1, samples)
    butt = pts[low]                                   # a crack's wedge reaches up the shingle: not the butt
    ys = []
    for x in xs:
        near = butt[np.abs(butt[:, 0] - x) < .02]
        ys.append(float(near[:, 1].min()) if len(near) else float(np.interp(x, *butt[np.argsort(butt[:, 0])].T)))
    return float(x0), float(x1), ys


def main(out):
    spec, _ = shingle_bake.layout(SEED)
    shingles = []
    for p in spec['pieces']:
        if p['kind'] != 'shingle':
            continue
        outline = json.loads(p['outline']) if isinstance(p['outline'], str) else p['outline']
        x0, x1, ys = butt_edge(outline)
        shingles.append({'course': p['course'], 'x': [round(x0, 4), round(x1, 4)],
                         'butt_y': [round(y, 4) for y in ys], 'butt_cm': round(100 * p['butt'], 2)})
    data = {'texture': 'NW_CITY_ROOF_01', 'set': 'roofs-8', 'tool': 'scripts/content/shingle_bake.py', 'seed': SEED,
            'period_m': spec['period'], 'course_lines_m': [round(v, 4) for v in spec['course_lines']],
            'y': 'metres up the slope from V = 1; V = 1 - y / period_m[1]', 'shingles': shingles}
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text(json.dumps(data, indent=0) + '\n')
    print(f'{out}: {len(shingles)} shingles, {len(spec["course_lines"])} courses')


if __name__ == '__main__':
    main(sys.argv[1])
