"""r7 sets: the r6 smoke (g75 a52) made denser so that it reads by day (the user, 1.11: «Сделай гуще, чтобы днём читался»).
usage: gen_r7.py OUT_DIR
r6 covers ~0.13 of the background in the throat by day. Optical depth adds: twice the particles and a higher alpha give
about x2, x3 and x4 of it, a coverage of ~0.25, ~0.35 and ~0.45 (1 - 0.87^k), still a veil, far from the r4 column (~1).
Each set is OUT_DIR/<set>/particles.d/harbour-smoke.json: the effects of data/content/particles.d with the fields below."""
import json, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[5]
base = json.loads((ROOT / 'data/content/particles.d/harbour-smoke.json').read_text())
SETS = {'r7-d2': dict(ppsValue=8.0, visAlphaStart=52.0),
        'r7-d3': dict(ppsValue=8.0, visAlphaStart=78.0),
        'r7-d4': dict(ppsValue=8.0, visAlphaStart=104.0)}
out = Path(sys.argv[1])
for name, fields in SETS.items():
    doc = json.loads(json.dumps(base))
    first = doc['effects'][0]
    assert first['name'] == 'OWN_HUT_SMOKE_04' and doc['effects'][1]['base'] == 'OWN_HUT_SMOKE_04'
    first['set'].update(fields)  # OWN_HUT_SMOKE_06 inherits them
    path = out / name / 'particles.d/harbour-smoke.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=1) + '\n')
    print(name, fields)
