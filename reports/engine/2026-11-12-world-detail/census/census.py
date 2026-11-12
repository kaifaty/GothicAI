"""Geometry census of a converted world (read-only).  usage: python3 census.py NEWWORLD  -> <world>_census.json + <world>_census.md"""
import os, sys, json, struct, time, collections
from pathlib import Path
import numpy as np
from classify import classify, strip
world=sys.argv[1]
RASTER=int(sys.argv[2]) if len(sys.argv)>2 else 1
BASE=Path(os.environ.get('OPENGOTHIC_BASE') or max((p for p in (Path.home()/'.cache'/'gothic-ai'/'base').iterdir() if (p/'base.json').exists()),key=lambda p:(p/'base.json').stat().st_mtime))
SEA=-7.0   # metres, NEWWORLD ocean plane
wj=json.load(open(BASE/'worlds'/world/'world.json'))
mats=wj['materials']
d=np.load(f'{world}.npz'); P=d['P'].astype(np.float64)/100.0; UV=d['UV'].astype(np.float64); M=d['M']; C=d['C']
nt=len(P)
cls_of=np.array([classify(m['name'],m['group']) for m in mats])
CL=cls_of[M]
n=np.cross(P[:,1]-P[:,0],P[:,2]-P[:,0]); nl=np.linalg.norm(n,axis=1)
A=nl/2; ny=np.divide(n[:,1],nl,out=np.zeros(nt),where=nl>0)
cen=P.mean(1)
wsel=np.nonzero(CL=='water')[0]
if len(wsel):
    wm=np.bincount(M[wsel],A[wsel],minlength=len(mats)).argmax(); SEA=float(np.median(cen[M==wm][:,1]))
else: SEA=-1e9
print('sea level (largest water material median y):',SEA)
under=cen[:,1]<SEA
out=[]; R={}
def tab(h,rows):
    s='| '+' | '.join(h)+' |\n|'+'|'.join(['---']*len(h))+'|\n'
    for r in rows: s+='| '+' | '.join(str(x) for x in r)+' |\n'
    return s+'\n'
# 1 totals
vpos=len(np.unique(P.astype(np.float32).reshape(-1,3),axis=0))
G=P.reshape(-1,3)*np.array([-1,1,1]); mn=G.min(0); mx=G.max(0)  # Gothic x = -glTF x
nchunks=len(list((BASE/'worlds'/world/'chunks').glob('*.glb')))
up=ny>0
plan_up=(A*ny)[up].sum()
R['totals']=dict(triangles=nt,vertices_world_json=wj['vertices'],unique_positions=vpos,bbox_min_m=mn.round(1).tolist(),bbox_max_m=mx.round(1).tolist(),chunks=nchunks,surface_area_m2=A.sum(),plan_upfacing_m2=plan_up,degenerate=int((A==0).sum()))
out.append(f'## {world}: totals\n\nSea level used to split land from seabed: y = {SEA:.1f} m (median centroid height of the largest water material).\n')
out.append(tab(['quantity','value'],[['triangles',f'{nt:,}'],['vertices (world.json, engine level)',f"{wj['vertices']:,}"],['unique positions (float32)',f'{vpos:,}'],
 ['bbox min x,y,z (m, Gothic axes)',mn.round(1).tolist()],['bbox max x,y,z (m, Gothic axes)',mx.round(1).tolist()],['64 m chunk files',nchunks],['surface area of all triangles (m2)',f'{A.sum():,.0f}'],
 ['plan area of up-facing triangles, sum(A*ny) (m2)',f'{plan_up:,.0f}'],['zero-area triangles',int((A==0).sum())]]))
# classes
CLS=['ground','rock','water','man-made','vegetation','other']
rows=[]
for c in CLS:
    s=CL==c
    # plan up
    rows.append([c,f'{s.sum():,}',f'{A[s].sum():,.0f}',f'{(A*ny)[s&up].sum():,.0f}',f'{s.sum()/max(A[s].sum(),1e-9):.3f}',f'{(s&under).sum():,}'])
out.append('### By material class\n\n'+tab(['class','triangles','area m2','plan (up-facing) m2','tris per m2','tris below sea (below sea level)'],rows))
R['classes']={c:dict(tris=int((CL==c).sum()),area=float(A[CL==c].sum()),tri_per_m2=float((CL==c).sum()/max(A[CL==c].sum(),1e-9))) for c in CLS}
# land-only (centroid above sea) for ground / rock
rows=[]
for c in ('ground','rock'):
    for lab,sel in (('land (above sea level)',~under),('below sea',under)):
        s=(CL==c)&sel
        rows.append([c,lab,f'{s.sum():,}',f'{A[s].sum():,.0f}',f'{s.sum()/max(A[s].sum(),1e-9):.3f}',f'{(A*ny)[s&up].sum():,.0f}'])
out.append('### Ground and rock split by sea level\n\n'+tab(['class','part','triangles','area m2','tris per m2','plan m2'],rows))
# edges
E=np.linalg.norm(P[:,[1,2,0]]-P,axis=2)  # (nt,3)
def pct(x,q=(10,50,90)): return [round(float(np.percentile(x,v)),2) for v in q]
rows=[]
for c in ('ground','rock','man-made','vegetation'):
    for lab,sel in (('all',np.ones(nt,bool)),('land',~under)):
        s=(CL==c)&sel
        if s.sum()==0: continue
        e=E[s].ravel(); ta=A[s]
        rows.append([c,lab,f'{s.sum():,}']+pct(e)+[round(float(np.median(ta)),3),round(float(np.sqrt(np.median(ta)*4/np.sqrt(3)*0+np.median(ta)*2)),2)])
out.append('### Edge length (m) and triangle area per class\n\n'+tab(['class','part','tris','edge p10','edge p50','edge p90','median tri area m2','sqrt(2*median area) m'],rows))
R['edges']={r[0]+'/'+r[1]:r[3:6] for r in rows}
# top 40 materials
ar=np.bincount(M,A,minlength=len(mats)); nm_=np.bincount(M,minlength=len(mats))
o=np.argsort(-ar)[:40]
out.append('### 40 largest materials by area\n\n'+tab(['material','group','class','tris','area m2','tris/m2','median edge m'],
  [[mats[i]['name'],mats[i]['group'],cls_of[i],f'{nm_[i]:,}',f'{ar[i]:,.0f}',f'{nm_[i]/ar[i]:.3f}',round(float(np.median(E[M==i])),2)] for i in o]))
# 3 chunks
cid=C[:,0].astype(np.int64)*100000+C[:,1]
u,inv=np.unique(cid,return_inverse=True)
ct=np.bincount(inv); ca=np.bincount(inv,A)
out.append(f'## {world}: per-chunk triangles\n\n'+tab(['stat','triangles/chunk','area m2/chunk'],[['chunks with triangles',len(u),''],['min',ct.min(),f'{ca.min():,.0f}'],['median',int(np.median(ct)),f'{np.median(ca):,.0f}'],['mean',int(ct.mean()),f'{ca.mean():,.0f}'],['p90',int(np.percentile(ct,90)),f'{np.percentile(ca,90):,.0f}'],['max',ct.max(),f'{ca.max():,.0f}']]))
R['chunks']=dict(n=len(u),min=int(ct.min()),median=float(np.median(ct)),p90=float(np.percentile(ct,90)),max=int(ct.max()))
top=np.argsort(-ct)[:12]
rows=[]
for k in top:
    s=inv==k; x,z=C[s][0]
    cc=collections.Counter(); 
    for c in CLS: cc[c]=A[s&(CL==c)].sum()
    mm=np.bincount(M[s],A[s],minlength=len(mats)); j=np.argsort(-mm)[:2]
    rows.append([f'{x}_{z}',f'{ct[k]:,}',f'{ca[k]:,.0f}',f'{ct[k]/ca[k]:.2f}',', '.join(f'{c} {cc[c]/ca[k]*100:.0f}%' for c in CLS if cc[c]>0.1*ca[k]),', '.join(mats[a]['name'] for a in j)])
out.append('### Densest 12 chunks by triangles\n\n'+tab(['chunk x_z','tris','area m2','tris/m2','class shares of area','top materials'],rows))
# 4 slope
bands=[(0,15),(15,30),(30,45),(45,60),(60,90)]
ang=np.degrees(np.arccos(np.clip(ny,-1,1)))
def slope_rows(sel):
    tot=A[sel].sum(); r=[]
    for lo,hi in bands:
        s=sel&(ny>=0)&(ang>=lo)&(ang<hi+(1e-9 if hi==90 else 0)); r.append(f'{A[s].sum()/tot*100:.1f}')
    s=sel&(ny<0)&(ny>=-0.5); r.append(f'{A[s].sum()/tot*100:.1f}')
    s=sel&(ny<-0.5); r.append(f'{A[s].sum()/tot*100:.1f}')
    return r+[f'{tot:,.0f}']
rows=[]
for c in ('ground','rock','man-made'):
    for lab,sel in (('all',np.ones(nt,bool)),('land',~under)):
        rows.append([c,lab]+slope_rows((CL==c)&sel))
out.append('### Slope bands, share of surface area in % (angle of the face normal from straight up; "leaning out" = normal y in [-0.5,0), i.e. 90-120 deg, vertical walls tilting down; "ceiling" = normal y < -0.5, i.e. overhang/underside steeper than 60 deg from the horizontal)\n\n'+tab(['class','part','0-15','15-30','30-45','45-60','60-90','leaning out','ceiling','area m2'],rows))
R['slope']={r[0]+'/'+r[1]:r[2:] for r in rows}
# 7 texel density (ground)
def ddsres(tex):
    f=BASE/'textures'/(Path(tex).stem+'.dds')
    if not f.exists(): return None
    h=f.open('rb').read(32); hh,ww=struct.unpack_from('<II',h,12); return ww,hh
uva=np.abs((UV[:,1,0]-UV[:,0,0])*(UV[:,2,1]-UV[:,0,1])-(UV[:,2,0]-UV[:,0,0])*(UV[:,1,1]-UV[:,0,1]))/2
gi=[i for i in np.argsort(-ar) if cls_of[i]=='ground'][:10]
rows=[]
for i in gi:
    s=(M==i)&(A>0.05)&(uva>0)
    res=ddsres(mats[i]['texture'])
    if res is None or s.sum()==0: rows.append([mats[i]['name'],mats[i]['texture'],'n/a','','','']); continue
    td=np.sqrt(uva[s]*res[0]*res[1]/A[s])
    rows.append([mats[i]['name'],f'{res[0]}x{res[1]}',f'{ar[i]:,.0f}',f'{np.median(td):.1f}',f'{np.percentile(td,10):.1f} / {np.percentile(td,90):.1f}',f'{np.median(np.sqrt(uva[s]/A[s])):.4f}'])
out.append(f'## {world}: texel density, ten largest ground materials (base .dds; texels per metre = sqrt(uv area x w x h / world area) per triangle, triangles > 0.05 m2)\n\n'+tab(['material','dds size','area m2','median texels/m','p10 / p90','median uv-units per m'],rows))
R['texel']=rows
# 5 raster
def raster(sel, cell=1.0):
    idx=np.nonzero(sel)[0]
    x=P[idx][:,:,0]/cell; z=P[idx][:,:,2]/cell; y=P[idx][:,:,1]
    i0=np.ceil(x.min(1)-0.5).astype(np.int64); i1=np.floor(x.max(1)-0.5).astype(np.int64)
    j0=np.ceil(z.min(1)-0.5).astype(np.int64); j1=np.floor(z.max(1)-0.5).astype(np.int64)
    nx=i1-i0+1; nz=j1-j0+1; ok=(nx>0)&(nz>0)
    idx,x,z,y,i0,j0,nx,nz=[a[ok] for a in (idx,x,z,y,i0,j0,nx,nz)]
    prod=nx*nz; order=np.argsort(prod); 
    cells=[];hs=[];tid=[]
    def do(sl,mx,mz):
        xx=x[sl];zz=z[sl];yy=y[sl]
        oi,oj=np.meshgrid(np.arange(mx),np.arange(mz),indexing='ij'); oi=oi.ravel();oj=oj.ravel()
        ci=i0[sl][:,None]+oi[None,:]; cj=j0[sl][:,None]+oj[None,:]
        inside=(oi[None,:]<nx[sl][:,None])&(oj[None,:]<nz[sl][:,None])
        px=ci+0.5; pz=cj+0.5
        x0,x1,x2=xx[:,0:1],xx[:,1:2],xx[:,2:3]; z0,z1,z2=zz[:,0:1],zz[:,1:2],zz[:,2:3]
        den=(z1-z2)*(x0-x2)+(x2-x1)*(z0-z2)
        den=np.where(den==0,np.nan,den)
        l0=((z1-z2)*(px-x2)+(x2-x1)*(pz-z2))/den
        l1=((z2-z0)*(px-x2)+(x0-x2)*(pz-z2))/den
        l2=1-l0-l1; e=-1e-9
        m=inside&(l0>=e)&(l1>=e)&(l2>=e)
        h=l0*yy[:,0:1]+l1*yy[:,1:2]+l2*yy[:,2:3]
        r,cidx=np.nonzero(m)
        cells.append(np.stack([ci[r,cidx],cj[r,cidx]],1)); hs.append(h[r,cidx]); tid.append(idx[sl][r])
    pos=0; N=len(order)
    while pos<N:
        # grow batch while candidate count stays <= 3e6
        end=pos+1
        step=256
        while end<N:
            nxt=min(N,end+step); sl=order[pos:nxt]
            if (nxt-pos)*int(nx[sl].max())*int(nz[sl].max())>3_000_000: 
                if step==1: break
                step//=2; continue
            end=nxt
        sl=order[pos:end]
        do(sl,int(nx[sl].max()),int(nz[sl].max()))
        pos=end
    return np.concatenate(cells),np.concatenate(hs),np.concatenate(tid)
def layers(sel,label):
    t=time.time()
    cl,h,tid=raster(sel)
    gx0=int(cl[:,0].min()); gz0=int(cl[:,1].min()); W=int(cl[:,1].max())-gz0+1
    key=(cl[:,0]-gx0)*W+(cl[:,1]-gz0)
    o=np.lexsort((h,key)); key=key[o];h=h[o];tid=tid[o]
    newcell=np.r_[True,key[1:]!=key[:-1]]
    newlayer=newcell|np.r_[False,(h[1:]-h[:-1])>2.0]
    # layer count per cell
    cellid=np.cumsum(newcell)-1
    nlay=np.bincount(cellid,newlayer.astype(np.int64))
    first=np.nonzero(newcell)[0]; last=np.r_[first[1:],len(key)]-1
    ckey=key[first]
    cx=ckey//W+gx0; cz=ckey%W+gz0
    print(label,'cells',len(first),'pairs',len(key),'s',round(time.time()-t,1),flush=True)
    return dict(nlay=nlay,cx=cx,cz=cz,lo=tid[first],hi=tid[last],hlo=h[first],hhi=h[last])
if RASTER:
    land=~under
    sets={'natural (ground+rock), land':(((CL=='ground')|(CL=='rock'))&land&(ny>0.1)),
          'solid (ground+rock+man-made), land':(((CL=='ground')|(CL=='rock')|(CL=='man-made'))&land&(ny>0.1)),
          'natural + seabed':(((CL=='ground')|(CL=='rock'))&(ny>0.1))}
    res={}
    rows=[]
    for k,sel in sets.items():
        L=layers(sel,k); res[k]=L; nl=L['nlay']; tot=len(nl)
        rows.append([k,f'{tot:,}',f'{(nl==1).sum():,} ({(nl==1).mean()*100:.1f}%)',f'{(nl>=2).sum():,} ({(nl>=2).mean()*100:.1f}%)',f'{(nl>=3).sum():,}'])
    out.append(f'## {world}: layers on a 1 m XZ grid (cell centre, up-facing triangles with normal y>0.1, heights chained: a gap > 2 m starts a new layer)\n\n'+tab(['set','cells with >=1 layer (= m2)','exactly 1 layer','>=2 layers (gap>2 m)','>=3 layers'],rows))
    R['layers']=rows
    # where
    for k in ('natural (ground+rock), land','solid (ground+rock+man-made), land'):
        L=res[k]; m=L['nlay']>=2
        loC=C[L['lo'][m]]; key=loC[:,0].astype(np.int64)*100000+loC[:,1]; uu,ii=np.unique(key,return_inverse=True); cnt=np.bincount(ii)
        gap=L['hhi'][m]-L['hlo'][m]
        hist=[(gap<=5).sum(),((gap>5)&(gap<=15)).sum(),(gap>15).sum()]
        capm=np.array([mats[M[i]]['name'].startswith('S:') and 'CAVE' in mats[M[i]]['name'] for i in L['lo'][m]])|np.array([mats[M[i]]['name'].startswith('S:') and 'CAVE' in mats[M[i]]['name'] for i in L['hi'][m]])
        out.append(f'Multi-layer cells ({k}): {m.sum():,}; gap between lowest and highest layer: <=5 m {hist[0]:,}, 5-15 m {hist[1]:,}, >15 m {hist[2]:,}; cells with an S:*CAVE* room material in the lowest or highest layer: {capm.sum():,}.\n')
        R['gaphist_'+k]=[int(x) for x in hist]+[int(capm.sum())]
        o=np.argsort(-cnt)[:20]; rows=[]
        for q in o:
            s=ii==q
            lo=L['lo'][m][s]; hi=L['hi'][m][s]
            def top(ids):
                c=collections.Counter(strip(mats[M[i]]['name']) for i in ids); return ', '.join(f'{a} {b}' for a,b in c.most_common(2))
            cave=sum(mats[M[i]]['name'].startswith('S:') and 'CAVE' in mats[M[i]]['name'] for i in np.r_[lo,hi])
            dh=(L['hhi'][m][s]-L['hlo'][m][s])
            ccx,ccz=C[L['lo'][m]][s][0]
            rows.append([f'{ccx}_{ccz}',cnt[q],f'{np.median(dh):.1f}',f'{cave/(2*s.sum())*100:.0f}%',top(lo),top(hi)])
        out.append(f'### Where: multi-layer cells, {k} - top 20 chunks (x_z = floor(metre/64); cave % = share of the two end layers with an S:*CAVE* material)\n\n'+tab(['chunk','multi-layer cells','median height gap m','cave mats','lowest layer materials','highest layer materials'],rows))
        tot_chunks=len(uu)
        out.append(f'Chunks with at least one multi-layer cell: {tot_chunks}; chunks with >= 500 cells: {(cnt>=500).sum()}.\n\n')
        R['where_'+k]=rows
    # raw dump for later
    np.savez(f'{world}_layers.npz',**{k.replace(' ','_')+'__'+f:v for k,L in res.items() for f,v in L.items()})
json.dump(R,open(f'{world}_census.json','w'),indent=1,default=float)
open(f'{world}_census.md','w').write('\n'.join(out))
print('done')
