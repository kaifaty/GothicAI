"""Own-smoke screening sets (Д2б): braziers of harbour-motion-2 + own PFX OWN_HARBOUR_SMOKE at the two roof holes,
tilted with the wind. Rounds o (texture and colour), p (light, wide, tilt), q (lighter, wider), r (cone, life),
s (life); the choice is own-s2, now in data/content/world/harbour-motion-placements.json.

usage: gen_own.py HARBOUR_MOTION_2_LAYER OUT_DIR [round2|round3|round4|round5]"""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[5] / 'scripts/content'))
import harbour_motion as hm
base = json.loads(Path(sys.argv[1]).read_text())
out = Path(sys.argv[2])
keep = [e for e in base['add'] if not e['key'].startswith('smoke-')]
H04 = [(2836, 245, -2426), (2871, 245, -2426), (2906, 245, -2426)]
H06 = [(3998, 262, -4061), (4038, 262, -4061)]
COMMON = {'visName_S': 'SMK_16BIT_A0.TGA', 'visTexAniFPS': 2.0, 'visTexAniIsLooping': 1,
          'visTexColorStart_S': '90 90 90', 'visTexColorEnd_S': '110 110 110',
          'visAlphaStart': 200.0, 'visAlphaEnd': 0.0, 'visSizeStart_S': '40 40', 'visSizeEndScale': 5.0,
          'lspPartAvg': 12000.0, 'lspPartVar': 1500.0, 'velAvg': 0.035, 'velVar': 0.008,
          'ppsValue': 8.0, 'dirAngleElevVar': 10.0, 'dirAngleHeadVar': 25.0}
V = {
 'own-o1': COMMON,
 'own-o2': {**COMMON, 'visTexColorStart_S': '60 60 60', 'visTexColorEnd_S': '80 80 80'},
 'own-o3': {**COMMON, 'visName_S': 'FIRESMOKE.TGA', 'visTexAniFPS': 0.0, 'visTexColorStart_S': '110 110 110',
            'visTexColorEnd_S': '130 130 130', 'visAlphaStart': 220.0},
 'own-o4': {**COMMON, 'ppsValue': 14.0},
 'own-o5': {**COMMON, 'visSizeStart_S': '60 60', 'visSizeEndScale': 6.0},
 'own-o6': {**COMMON, 'visTexColorStart_S': '140 140 140', 'visTexColorEnd_S': '160 160 160'},
}
TILT = {}
HOLES = {}
if len(sys.argv) > 3 and sys.argv[3] == 'round2':
    LIGHT = {**COMMON, 'visTexColorStart_S': '140 140 140', 'visTexColorEnd_S': '160 160 160',
             'visSizeStart_S': '60 60', 'visSizeEndScale': 6.0}
    V = {'own-p1': LIGHT, 'own-p2': LIGHT,
         'own-p3': {**LIGHT, 'visTexColorStart_S': '100 100 100', 'visTexColorEnd_S': '170 170 170'},
         'own-p4': {**LIGHT, 'ppsValue': 12.0},
         'own-p5': {**LIGHT, 'lspPartAvg': 15000.0, 'velAvg': 0.04},
         'own-p6': LIGHT}
    TILT = {'own-p1': 40}
    H04_4 = [(2816, 245, -2426), (2853, 245, -2426), (2889, 245, -2426), (2926, 245, -2426)]
    H06_3 = [(3993, 262, -4061), (4018, 262, -4061), (4043, 262, -4061)]
    HOLES = {'own-p6': (H04_4, H06_3)}
if len(sys.argv) > 3 and sys.argv[3] == 'round3':
    P5 = {**COMMON, 'visTexColorStart_S': '140 140 140', 'visTexColorEnd_S': '160 160 160',
          'visSizeStart_S': '60 60', 'visSizeEndScale': 6.0, 'lspPartAvg': 15000.0, 'velAvg': 0.04}
    Q2 = {**P5, 'visTexColorStart_S': '170 170 170', 'visTexColorEnd_S': '190 190 190'}
    V = {'own-q1': P5, 'own-q2': Q2,
         'own-q3': {**P5, 'visTexColorStart_S': '190 190 190', 'visTexColorEnd_S': '210 210 210'},
         'own-q4': {**Q2, 'ppsValue': 12.0},
         'own-q5': {**Q2, 'visSizeStart_S': '80 80'},
         'own-q6': Q2}
    TILT = {'own-q6': 0}
if len(sys.argv) > 3 and sys.argv[3] == 'round4':
    Q5 = {**COMMON, 'visTexColorStart_S': '170 170 170', 'visTexColorEnd_S': '190 190 190',
          'visSizeStart_S': '80 80', 'visSizeEndScale': 6.0, 'lspPartAvg': 15000.0, 'velAvg': 0.04}
    R2 = {**Q5, 'dirAngleElevVar': 20.0, 'velVar': 0.015}
    V = {'own-r1': Q5, 'own-r2': R2,
         'own-r3': {**R2, 'visTexAniFPS': 6.0},
         'own-r4': {**R2, 'lspPartAvg': 12000.0},
         'own-r5': {**R2, 'ppsValue': 6.0}}
if len(sys.argv) > 3 and sys.argv[3] == 'round5':
    R4 = {**COMMON, 'visTexColorStart_S': '170 170 170', 'visTexColorEnd_S': '190 190 190',
          'visSizeStart_S': '80 80', 'visSizeEndScale': 6.0, 'lspPartAvg': 12000.0, 'velAvg': 0.04,
          'dirAngleElevVar': 20.0, 'velVar': 0.015}
    V = {'own-s1': R4, 'own-s2': {**R4, 'lspPartAvg': 13500.0}, 'own-s3': {**R4, 'ppsValue': 10.0}}
for name, fields in V.items():
    tilt = TILT.get(name, 25 if name[:5] in ('own-p', 'own-q', 'own-r', 'own-s') else 40)
    h04, h06 = HOLES.get(name, (H04, H06))
    items = [dict(key=f'smoke-hut04-{i+1}', kind='emitter', pfx='OWN_HARBOUR_SMOKE', position=list(p), tilt=tilt) for i, p in enumerate(h04)]
    items += [dict(key=f'smoke-hut06-{i+1}', kind='emitter', pfx='OWN_HARBOUR_SMOKE', position=list(p), tilt=tilt) for i, p in enumerate(h06)]
    smoke = hm.layer_entries(items)
    d = out / name; (d / 'particles.d').mkdir(parents=True, exist_ok=True)
    (d / 'harbour-motion.json').write_text(json.dumps(dict(format=1, add=smoke + keep), indent=1) + '\n')
    (d / 'particles.d/harbour-smoke.json').write_text(json.dumps(hm.particle_document(
        [dict(name='OWN_HARBOUR_SMOKE', base='LIGHTSMOKE', set=fields)]), indent=1) + '\n')
    print(name, len(smoke), 'emitters')
