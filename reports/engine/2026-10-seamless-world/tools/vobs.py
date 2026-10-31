import json,os,itertools,collections
import sys
# usage: python3 <script> <base worlds dir, e.g. ~/.cache/gothic-ai/base/<fingerprint>/worlds>
B=os.path.expanduser(sys.argv[1])
W=['NEWWORLD','OLDWORLD','ADDONWORLD','DRAGONISLAND']
spots={};vnames={};cls={}
def walk(vs,out):
    for v in vs:
        out.append(v); walk(v.get('children') or [],out)
for w in W:
    d=json.load(open(f'{B}/{w}/vobs.json'))
    allv=[];walk(d['vobs'],allv)
    c=collections.Counter(v['class'] for v in allv); cls[w]=c
    spots[w]=set(v['vob_name'].upper() for v in allv if 'Spot' in v['class'] and v.get('vob_name'))
    vnames[w]=set(v['vob_name'].upper() for v in allv if v.get('vob_name') and 'Spot' not in v['class'])
    print(w,'vobs',len(allv),'spots',len(spots[w]),'named',len(vnames[w]), dict(c.most_common(25)))
    for v in allv:
        if 'ChangeLevel' in v['class'] or 'LevelChange' in v['class']:
            print('   CHANGELEVEL',v.get('vob_name'),[round(x/100) for x in v['position']], {k:v.get(k) for k in v if k in('level_name','start_vob','start_vob_name','level','vob_start')} , [k for k in v.keys()][-6:])
for a,b in itertools.combinations(W,2):
    s=spots[a]&spots[b]; n=vnames[a]&vnames[b]
    print('spot collide',a,b,len(s),sorted(s)[:10]); print('  name collide',len(n),sorted(n)[:15])
