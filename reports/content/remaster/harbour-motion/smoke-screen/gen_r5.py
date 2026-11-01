"""r5 smoke sets: one planned run by the research card (reports/Дым очага в игровом мире.md), not a search.
usage: gen_r5.py HARBOUR_MOTION_3_LAYER OUT_DIR
Every set is harbour-motion-3 with its smoke replaced: one emitter at the mouth of each hole (centroid of the
opening in the roofholes-2 mesh, on the plane of its rim), OWN_HUT_SMOKE_04 / _06 with keys in another phase."""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[5] / 'scripts/content'))
import harbour_motion as hm
base = json.loads(Path(sys.argv[1]).read_text()); out = Path(sys.argv[2])
keep = [e for e in base['add'] if not e['key'].startswith('smoke-')]
HOLES = {  # centroid of the opening, rim plane height there; source as large as the opening (CIRCLE scales per axis)
    '04': dict(position=[2859, 207, -2428], shpDim_S='30 0 12', keys='0.7 1 1.3 1 0.8 1.2'),   # 120 x 37 cm along x
    '06': dict(position=[4005, 257, -4054], shpDim_S='15 0 10', keys='1 0.8 1.2 0.7 1 1.3'),   # 41 x 23 cm
}
CARD = {'visName_S': 'OWN_HUT_SMOKE.TGA', 'visTexAniFPS': 0.0, 'visTexAniIsLooping': 0, 'visAlphaFunc_S': 'BLEND',
        'visSizeStart_S': '70 70', 'visSizeEndScale': 5.0, 'visAlphaEnd': 0.0,
        'lspPartAvg': 10000.0, 'lspPartVar': 1000.0,
        'ppsValue': 4.0, 'ppsIsLooping': 1, 'ppsIsSmooth': 1, 'ppsFPS': 0.25,
        'shpType_S': 'CIRCLE', 'shpIsVolume': 1, 'shpOffsetVec_S': '0 30 0',
        'dirMode_S': 'DIR', 'dirAngleElev': 90.0, 'dirAngleElevVar': 10.0, 'dirAngleHead': 0.0, 'dirAngleHeadVar': 179.0,
        'velAvg': 0.075, 'velVar': 0.015, 'flyGravity_S': '0 0 0.00001'}
GREYS = {'g75': ('75 72 66', '85 82 76'), 'g95': ('95 91 84', '105 101 94'), 'g115': ('115 111 104', '125 121 114')}
ALPHAS = {'a29': 29.0, 'a52': 52.0}  # 25 and 45 of the card over the texture's peak alpha 0.867
SETS = {f'r5-{g}{a}': (GREYS[g], ALPHAS[a]) for g in GREYS for a in ALPHAS}
SETS.update({f'r5-k{a[1:]}': (('0 0 0', '0 0 0'), ALPHAS[a]) for a in ALPHAS})  # black probes: coverage = 1 - O/A
for name, ((start, end), alpha) in SETS.items():
    items, effects = [], []
    for hut, hole in HOLES.items():
        items.append(dict(key=f'smoke-hut{hut}', kind='emitter', pfx=f'OWN_HUT_SMOKE_{hut}', position=hole['position']))
        effects.append(dict(name=f'OWN_HUT_SMOKE_{hut}', base='LIGHTSMOKE', set={
            **CARD, 'visTexColorStart_S': start, 'visTexColorEnd_S': end, 'visAlphaStart': alpha,
            'shpDim_S': hole['shpDim_S'], 'ppsScaleKeys_S': hole['keys']}))
    d = out / name; (d / 'particles.d').mkdir(parents=True, exist_ok=True)
    (d / 'harbour-motion.json').write_text(json.dumps(dict(format=1, add=hm.layer_entries(items) + keep), indent=1) + '\n')
    (d / 'particles.d/harbour-smoke.json').write_text(json.dumps(hm.particle_document(effects), indent=1) + '\n')
    print(name)
