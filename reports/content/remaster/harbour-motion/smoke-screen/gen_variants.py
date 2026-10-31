"""Smoke screening layers: the braziers of harbour-motion-2 plus other smoke at the two roof holes.

usage: gen_variants.py HARBOUR_MOTION_2_LAYER OUT_DIR [round2]"""
import json, sys
from pathlib import Path
base = json.loads(Path(sys.argv[1]).read_text())
out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
keep = [e for e in base['add'] if not e['key'].startswith('smoke-')]
# hole HAFEN04: 146 x 40 cm along x, centre (2871, -2426), emitters 10 cm under its top edge (y 245)
# hole HAFEN06: 73 x 54 cm, centre (4018, -4061), y 262
H04, H06 = (2871, 245, -2426), (4018, 262, -4061)
def row(hole, n, span, dz=0):
    x, y, z = hole
    if n == 1:
        return [(x, y, z)]
    return [(round(x - span / 2 + span * i / (n - 1)), y, z + (dz if i % 2 else -dz)) for i in range(n)]
import math
def em(name, pfx, pts, tilt=0.0):
    # tilt: the emitter's up axis leans towards +z, the world wind (common/physics/worldwind.h)
    c, s = math.cos(math.radians(tilt)), math.sin(math.radians(tilt))
    turn = dict(rotation=[[1, 0, 0], [0, round(c, 6), round(-s, 6)], [0, round(s, 6), round(c, 6)]]) if tilt else dict(yaw_deg=0.0)
    return [dict(key=f'smoke-{name}-{i + 1}', kind='emitter', position=[float(v) for v in p], **turn, pfx=pfx)
            for i, p in enumerate(pts)]
LS04, LS06 = row(H04, 3, 70), row(H06, 2, 40)
V = {
 'smoke-s0': em('hut04', 'LIGHTSMOKE', LS04) + em('hut06', 'LIGHTSMOKE', LS06),
 'smoke-s1': em('hut04', 'LIGHTSMOKE', row(H04, 8, 126, 10)) + em('hut06', 'LIGHTSMOKE', row(H06, 5, 56, 12)),
 'smoke-s2': em('hut04', 'LIGHTSMOKE', LS04) + em('hut06', 'LIGHTSMOKE', LS06)
             + em('hut04-base', 'FIRE_SMOKE', row(H04, 3, 90)) + em('hut06-base', 'FIRE_SMOKE', row(H06, 2, 30)),
 'smoke-s3': em('hut04', 'LIGHTSMOKE', LS04) + em('hut06', 'LIGHTSMOKE', LS06)
             + em('hut04-base', 'SMOKE1', row(H04, 1, 0)) + em('hut06-base', 'SMOKE1', row(H06, 1, 0)),
 'smoke-s4': em('hut04', 'LIGHTSMOKE', LS04) + em('hut06', 'LIGHTSMOKE', LS06)
             + em('hut04-base', 'WATERVAPOUR', row(H04, 3, 90)) + em('hut06-base', 'WATERVAPOUR', row(H06, 2, 30)),
 'smoke-s5': em('hut04', 'LIGHTSMOKE', row(H04, 8, 126, 10)) + em('hut06', 'LIGHTSMOKE', row(H06, 5, 56, 12))
             + em('hut04-base', 'FIRE_SMOKE', row(H04, 3, 90)) + em('hut06-base', 'FIRE_SMOKE', row(H06, 2, 30)),

 'smoke-s6': em('hut04', 'LIGHTSMOKE', row(H04, 8, 126, 10), 40) + em('hut06', 'LIGHTSMOKE', row(H06, 5, 56, 12), 40)
             + em('hut04-base', 'FIRE_SMOKE', row(H04, 3, 90)) + em('hut06-base', 'FIRE_SMOKE', row(H06, 2, 30)),
}
def rows2(hole, n, span, dz):
    # two rows along the long axis of the hole, n emitters each
    x, y, z = hole
    return [(round(x - span / 2 + span * i / max(n - 1, 1)), y, z + side) for side in (-dz, dz) for i in range(n)]
if len(sys.argv) > 3 and sys.argv[3] == 'round2':
    V = {
     'smoke-t1': em('hut04', 'LIGHTSMOKE', row(H04, 8, 126, 10), 40) + em('hut06', 'LIGHTSMOKE', row(H06, 5, 56, 12), 40),
     'smoke-t2': em('hut04', 'LIGHTSMOKE', rows2(H04, 8, 126, 10), 40) + em('hut06', 'LIGHTSMOKE', rows2(H06, 5, 56, 12), 40),
     'smoke-t3': em('hut04', 'LIGHTSMOKE', rows2(H04, 12, 126, 10), 40) + em('hut06', 'LIGHTSMOKE', rows2(H06, 8, 56, 12), 40),
     'smoke-t4': em('hut04', 'LIGHTSMOKE', rows2(H04, 8, 126, 10), 60) + em('hut06', 'LIGHTSMOKE', rows2(H06, 5, 56, 12), 60),
     'smoke-t5': em('hut04', 'LIGHTSMOKE', rows2(H04, 8, 126, 10), 40) + em('hut06', 'LIGHTSMOKE', rows2(H06, 5, 56, 12), 40)
                 + em('hut04-base', 'WATERVAPOUR', row(H04, 3, 90)) + em('hut06-base', 'WATERVAPOUR', row(H06, 2, 30)),
    }
for name, smoke in V.items():
    d = out / name; d.mkdir(exist_ok=True)
    (d / 'harbour-motion.json').write_text(json.dumps(dict(format=1, add=smoke + keep), indent=1) + '\n')
    print(name, len(smoke), 'smoke emitters', sorted({e['pfx'] for e in smoke}))
