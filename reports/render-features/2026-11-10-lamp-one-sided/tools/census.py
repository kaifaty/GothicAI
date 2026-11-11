#!/usr/bin/env python3
"""The lamp census of 2026-11-10: views round every lamp cluster of the city at night, each shot twice in one frame
(`vsm sides 1` and `vsm sides 2`), and the views of a ranking against the path tracer.

census.py lamps <base>/worlds/NEWWORLD OUT        -> OUT/clusters.json: the city's lamps as the engine lights them
                                                   (common/world/voblight.cpp), within 15 m of a NW_CITY waypoint,
                                                   merged within 3 m
census.py bench OUT [N]                          -> OUT/lamp-census.bench, OUT/meta.json: 6 views a cluster (4 round it
                                                   at the lamp's height, 2 3.3 m lower, under a floor)
census.py verify OUT ranking.json SIDES...       -> OUT/verify.bench: the 40 views darkened most, 30 more at random,
                                                   the 10 brightened most; shots <view>-<sides> and <view>-pt
Run the benches with TEMPEST_WINDOW_SIZE=960x540 OPENGOTHIC_LIGHT_FREEZE=1 OPENGOTHIC_TAA=0 OPENGOTHIC_GI=0
(OPENGOTHIC_RT=1 for the tracer) BENCH_HOST=ryzen scripts/bench.sh <bench> <out>.
"""
import json, math, random, sys


def lamps(world, out):
    pts = json.load(open(world + '/waynet.json'))['waynet']['points']
    city = [p['position'] for p in pts if p['name'].startswith('NW_CITY')]
    lights = []

    def rec(v):
        if v.get('class') == 'zCVobLight':
            lights.append(v)
        for c in v.get('children') or []:
            rec(c)
    for v in json.load(open(world + '/vobs.json'))['vobs']:
        rec(v)

    def lit(l):
        p = l.get('preset') or ''
        if not l['is_static']:
            return l['on']
        return not (p in ('NW_STANDART_DARKBLUE', 'DEFAULTLIGHT_DARKBLUE', 'INROOM_DARKBLUE', 'SCHMUTZ')
                    or p.startswith('AMBIENCE_') or l['range'] > 3000)

    def near(p):
        return any((p[0]-q[0])**2 + (p[2]-q[2])**2 < 1500**2 and abs(p[1]-q[1]) < 1500 for q in city)
    cls = []
    for l in sorted((l for l in lights if lit(l) and near(l['position'])), key=lambda l: l['position']):
        for c in cls:
            if math.dist(c['pos'], l['position']) < 300:
                c['n'] += 1
                break
        else:
            cls.append(dict(pos=l['position'], range=l['range'], n=1, preset=l.get('preset')))
    json.dump(cls, open(out + '/clusters.json', 'w'))
    print(len(cls), 'clusters')


def pair(o, name, sides):
    for s in sides:
        o += [f'vsm sides {s}', 'wait 350', f'shot {name}-{s}', 'wait 150']


def bench(out, limit):
    cls = json.load(open(out + '/clusters.json'))
    left = list(range(len(cls)))
    route = [left.pop(0)]
    while left:  # a greedy route, so that streaming stays local
        p = cls[route[-1]]['pos']
        j = min(left, key=lambda i: (cls[i]['pos'][0]-p[0])**2 + (cls[i]['pos'][2]-p[2])**2)
        left.remove(j)
        route.append(j)
    o = ['# @timeout 5400', 'seed 73013', 'cheat god', 'overlay off', 'ui off', 'view full', 'cam lift off',
         'view hide animated morph pfx', 'set time 0 0']
    meta = []
    for k, i in enumerate(route[:limit]):
        c = cls[i]
        x, y, z = c['pos']
        d = max(200, min(500, 0.7*c['range']))
        o += [f'goto pos {x:.0f} {y:.0f} {z:.0f}', 'wait 1000']
        views = [(f'h{a}', (x + d*math.cos(math.radians(a)), y - 40, z + d*math.sin(math.radians(a))), (x, y - 120, z))
                 for a in (0, 90, 180, 270)]
        views += [(f'l{a}', (x + 200*math.cos(math.radians(a)), y - 330, z + 200*math.sin(math.radians(a))),
                   (x - 150*math.cos(math.radians(a)), y - 360, z - 150*math.sin(math.radians(a)))) for a in (45, 225)]
        for tag, org, tgt in views:
            name = f'c{k:03d}-{tag}'
            o.append('cam at %.0f %.0f %.0f %.0f %.0f %.0f' % (*org, *tgt))
            pair(o, name, (1, 2))
            meta.append(dict(name=name, cluster=k, lamp=c, org=org, tgt=tgt))
    open(out + '/lamp-census.bench', 'w').write('\n'.join(o) + '\n')
    json.dump(meta, open(out + '/meta.json', 'w'))
    print(len(meta), 'views')


def verify(out, ranking, sides):
    meta = {m['name']: m for m in json.load(open(out + '/meta.json'))}
    rows = [r for r in json.load(open(ranking)) if r['shot'] in meta]
    random.seed(7)
    pick = rows[:40] + random.sample([r for r in rows[40:] if r['darker'] >= 30], 30)
    pick += sorted(rows, key=lambda r: -r['brighter'])[:10]
    names = sorted({r['shot'] for r in pick})
    o = ['# @timeout 3600', 'seed 73013', 'cheat god', 'overlay off', 'ui off', 'view full', 'cam lift off',
         'view hide animated morph pfx', 'set time 0 0']
    last = None
    for n in names:
        m = meta[n]
        if m['cluster'] != last:
            o += ['goto pos %.0f %.0f %.0f' % tuple(m['lamp']['pos']), 'wait 1500']
            last = m['cluster']
        o.append('cam at %.0f %.0f %.0f %.0f %.0f %.0f' % (*m['org'], *m['tgt']))
        pair(o, n, sides)
        o += ['vsm sides 0', 'toggle pathtrace', 'wait 8000', f'shot {n}-pt', 'wait 200', 'toggle pathtrace', 'wait 300']
    open(out + '/verify.bench', 'w').write('\n'.join(o) + '\n')
    print(len(names), 'views')


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'lamps':
        lamps(sys.argv[2], sys.argv[3])
    elif cmd == 'bench':
        bench(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 10**9)
    elif cmd == 'verify':
        verify(sys.argv[2], sys.argv[3], [int(s) for s in sys.argv[4:]] or [1, 2])
