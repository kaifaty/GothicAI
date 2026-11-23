import json,os
# reads ../measurements/<WORLD>_tric.json written by ext2.py (triangles per 64 m chunk)
S=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','measurements','')
T={w:{tuple(map(int,k.split('_'))):v for k,v in json.load(open(S+w+'_tric.json'))['tric'].items()} for w in ['NEWWORLD','OLDWORLD','ADDONWORLD']}
def overlap(a,b,off,thr=1):
    ox,oz=off  # in chunks
    A={k for k,v in T[a].items() if v>=thr}; Bs={(k[0]+ox,k[1]+oz) for k,v in T[b].items() if v>=thr}
    return len(A&Bs)
# offsets from portals (metres)-> chunks
for name,b,off in [('OW at pass',  'OLDWORLD',(240.4,-871.7)),('OW at mine','OLDWORLD',(231.6,-620)),('ADDON at portal','ADDONWORLD',(779,315))]:
    oc=(round(off[0]/64),round(off[1]/64))
    print(name,'offset chunks',oc,'overlap chunks any',overlap('NEWWORLD',b,oc),'dense(>=200)',overlap('NEWWORLD',b,oc,200))
# minimal Z shift for OW south of NW with X aligned at pass, no overlap of any chunk / dense chunk
for thr in (1,200):
  for dz in range(0,-40,-1):
    if overlap('NEWWORLD','OLDWORLD',(4,dz),thr)==0: print('OW X+4 chunks: first non-overlap dz',dz,'chunks =',dz*64,'m thr',thr); break
for thr in (1,200):
  for dx in range(0,40):
    if overlap('NEWWORLD','ADDONWORLD',(dx,5),thr)==0: print('ADDON Z+5: first non-overlap dx',dx,dx*64,'m thr',thr); break
# ascii map of NW (N), OW at pass offset (O), overlap (#)
def amap(b,oc,thr=200):
    A={k for k,v in T['NEWWORLD'].items() if v>=thr}; Bs={(k[0]+oc[0],k[1]+oc[1]) for k,v in T[b].items() if v>=thr}
    ks=A|Bs; xs=[k[0] for k in ks]; zs=[k[1] for k in ks]
    for z in range(max(zs),min(zs)-1,-1):
        print('%4d '%(z*64)+''.join('#' if (x,z) in A and (x,z) in Bs else 'N' if (x,z) in A else 'o' if (x,z) in Bs else '.' for x in range(min(xs),max(xs)+1)))
    print('x from',min(xs)*64)
print('--- NW + OW aligned at pass trigger (dense chunks)'); amap('OLDWORLD',(4,-14))
print('--- NW + ADDON aligned at portal (dense chunks)'); amap('ADDONWORLD',(12,5))
