#!/usr/bin/env python3
"""Pairs "objects one-sided | two-sided" (`vsm sides 2 | 3`) at the lamp views of gi-reference and at census views.

pairs.py player META OUT.bench -> as the player sees: TAA, the hero and flames, a bare shot without them
pairs.py ref META OUT.bench    -> bare shots and the path tracer, 12 s each (OPENGOTHIC_RT=1 OPENGOTHIC_GI=1
                                  OPENGOTHIC_TAA=0, the tracer with two-sided shadow rays: a study patch)
META is meta.json of the census of 2026-11-10 (census.py bench; /mnt/shared/gothic-ai/lamp-one-sided/2026-11-10/scripts).
"""
import json, sys
mode, meta_path, out_path = sys.argv[1:4]
meta = {m['name']: m for m in json.load(open(meta_path))}
spots = ['c233-h270', 'c163-h90', 'c207-h180', 'c166-h90', 'c058-h180', 'c232-h90', 'c183-h180', 'c269-h0', 'c049-h90']
o = ['# @timeout 2400', f'# lamp objects two-sided pairs ({mode}): vsm sides 2 | 3', 'seed 73013', 'cheat god', 'overlay off',
     'ui off', 'view full', 'cam lift off']
def shots(name):
    global o
    if mode == 'player':
        for sides, tag in ((2, 'a'), (3, 'b')):
            o += [f'vsm sides {sides}', 'wait 2000', f'shot {name}-{tag}', 'wait 200', 'view hide animated morph pfx',
                  'wait 1500', f'shot {name}-bare-{tag}', 'wait 200', 'view full']
    else:
        o += ['view hide animated morph pfx']
        for sides, tag in ((2, 'a'), (3, 'b')):
            o += [f'vsm sides {sides}', 'wait 3000', f'shot {name}-{tag}', 'wait 200']
        o += ['vsm sides 0', 'toggle pathtrace', 'wait 12000', f'shot {name}-pt', 'wait 200', 'toggle pathtrace',
              'wait 300', 'view full']
def view(time, place, look, world, name, first=True):
    global o
    if first:
        o += [f'set time {time}', f'goto {place}', 'clear 3000', 'watch player', f'cam look {look}', f'cam world {world}',
              'wait 10000', 'cam hold']
    else:
        o += [f'cam world {world}', 'wait 300', 'cam hold']
    shots(name)
if mode == 'ref':
    # the first path-traced shot of a run comes out black: a warm-up
    o += ['set time 22 0', 'goto pos 3924 -73 -6663', 'clear 3000', 'wait 5000', 'toggle pathtrace', 'wait 6000',
          'shot warmup-pt', 'toggle pathtrace', 'wait 300']
view('22 0', 'pos 3924 -73 -6663', 110, '160 170 270', 'interior-night-1')
view('22 0', '', 110, '160 170 180', 'interior-night-2', first=False)
view('23 0', 'NW_CITY_HABOUR_POOR_AREA_HUT_01_IN', 110, '180 170 0', 'hut-night-1')
view('0 0', 'NW_CITY_HABOUR_POOR_AREA_PATH_06', 120, '500 200 270', 'harbour-night-1')
view('0 0', '', 120, '500 200 180', 'harbour-night-2', first=False)
view('23 0', 'pos 1950 -120 -4700', 110, '200 150 90', 'leak-lamp-night')
for v in spots:
    m = meta[v]; x, y, z = m['lamp']['pos']; c = m['org'] + m['tgt']
    o += ['set time 0 0', f'goto pos {x:.0f} {y:.0f} {z:.0f}', 'clear 3000', 'wait 8000',
          'cam at %.0f %.0f %.0f %.0f %.0f %.0f' % tuple(c), 'wait 2000']
    shots(v)
open(out_path, 'w').write('\n'.join(o) + '\n')
print(out_path, sum(1 for x in o if x.startswith('shot')))
