import json,struct,os,array
import sys
# usage: python3 <script> <base worlds dir, e.g. ~/.cache/gothic-ai/base/<fingerprint>/worlds>
B=os.path.expanduser(sys.argv[1])
W=['NEWWORLD','OLDWORLD','ADDONWORLD','DRAGONISLAND']
def pct(a,p): a=sorted(a); return a[int(p*(len(a)-1))]
names={}
for w in W:
    wj=json.load(open(f'{B}/{w}/world.json'))
    X=[];Y=[];Z=[]
    tric={}
    for c in wj['chunks']:
        with open(f'{B}/{w}/{c}','rb') as f:
            data=f.read()
        L=struct.unpack('<I',data[12:16])[0]; j=json.loads(data[20:20+L])
        boff=20+L+8
        bin_=data[boff:]
        acc=j['accessors'][j['meshes'][0]['primitives'][0]['attributes']['POSITION']]
        bv=j['bufferViews'][acc['bufferView']]
        o=bv.get('byteOffset',0)+acc.get('byteOffset',0)
        arr=array.array('f'); arr.frombytes(bin_[o:o+acc['count']*12])
        X+= [-v for v in arr[0::3]]; Y+=list(arr[1::3]); Z+=list(arr[2::3])
        nt=0
        for m in j['meshes']:
            for pr in m['primitives']: nt+=j['accessors'][pr['indices']]['count']//3
        tric[os.path.basename(c)[:-4]]=nt
    print(w,'verts',len(X))
    for nm,a in (('X',X),('Y',Y),('Z',Z)):
        print('  %s m: min %.0f p1 %.0f p99 %.0f max %.0f'%(nm,min(a)/100,pct(a,.01)/100,pct(a,.99)/100,max(a)/100))
    big=[k for k,v in tric.items() if v>=200]
    xs=[int(k.split('_')[0]) for k in big]; zs=[int(k.split('_')[1]) for k in big]
    print('  chunks>=200 tris:',len(big),'X m %d..%d Z m %d..%d'%(min(xs)*64,(max(xs)+1)*64,min(zs)*64,(max(zs)+1)*64))
    wn=json.load(open(f'{B}/{w}/waynet.json'))['waynet']
    pts=wn['points']
    px=[p['position'][0] for p in pts]; pz=[p['position'][2] for p in pts]; py=[p['position'][1] for p in pts]
    print('  waypoints',len(pts),'edges',len(wn.get('edges',[])),'X m %.0f..%.0f Y %.0f..%.0f Z %.0f..%.0f'%(min(px)/100,max(px)/100,min(py)/100,max(py)/100,min(pz)/100,max(pz)/100))
    names[w]=set(p['name'].upper() for p in pts)
    json.dump({'tric':tric},open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','measurements',f'{w}_tric.json'),'w'))
import itertools
for a,b in itertools.combinations(W,2):
    s=names[a]&names[b]
    print('collide',a,b,len(s),sorted(s)[:15])
