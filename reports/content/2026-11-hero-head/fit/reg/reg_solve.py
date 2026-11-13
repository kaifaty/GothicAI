# Landmark-guided non-rigid ICP (Amberg et al. 2007) of the MPFB head skin onto the generator's reference.
#   python reg_solve.py <reg dir> [schedule.json]
# Reads mpfb.npz, ref.npz, views.json, lm.json from <reg dir>; writes reg.npz (new positions of all MPFB vertices,
# eyeball shift per eye) and reg_log.json. Units inside: cm around the MPFB eye centre.
import sys, os, json, time
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import splu
from scipy.spatial import cKDTree
from scipy import ndimage
D=sys.argv[1]; J=lambda n: json.load(open(os.path.join(D,n)))
m=np.load(os.path.join(D,'mpfb.npz')); r=np.load(os.path.join(D,'ref.npz')); V=J('views.json'); LM=J('lm.json')
sched=J(sys.argv[2]) if len(sys.argv)>2 else {'alpha':[40,20,10,5,3,2],'beta':[10,6,4,3,2,2],'iters':8,'dmax':1.5,'cos':0.5}
eye=np.array(V['eye']); S=V['s']; N=V['N']
g=np.load(os.path.join(D,'groups.npz')); co=m['co'].astype(np.float64); tris=m['tris']; edges=m['edges']; helper=m['helper']
LF=None
if os.path.exists(os.path.join(D,'lm_fixed.json')):                 # landmarks by MPFB vertex id, kept in git
    _l=J('lm_fixed.json')['landmarks']; LF={'names':np.array([x['name'] for x in _l]),'src':np.array([x['mpfb_vertex'] for x in _l]),'tgt':np.array([x['target'] for x in _l])}
eye_info={}
if sched.get('eye_block') and LF is not None:
    from scipy.optimize import lsq_linear
    E=np.load(os.path.join(D,'eye_deltas.npz')); tn=list(E['names']); DD=E['D'].astype(np.float64)
    ln=list(LF['names']); src0=LF['src']; tgt0=LF['tgt']
    skip=sched.get('eye_skip',['push'])                                                   # depth is the block's job
    lam=sched.get('eye_lam',0.3); ax=[0,2]                                                # fit the lids as seen from the front
    def eq(side,pre):
        idx=[i for i,n_ in enumerate(ln) if n_.startswith(pre)]
        ti=[k for k,n_ in enumerate(tn) if n_.startswith(side+'-') and not any(x in n_ for x in skip)]
        A=np.concatenate([DD[ti][:,src0[i],:][:,ax].T for i in idx])*1000; b=np.concatenate([(tgt0[i]-co[src0[i]])[ax] for i in idx])*1000
        return ti,A,b
    if sched.get('eye_sym'):                                                              # one set of values for both eyes
        (tl,Al,bl),(tr,Ar,br)=eq('l','l_'),eq('r','r_'); nl=[tn[k][2:] for k in tl]; nr=[tn[k][2:] for k in tr]
        com=[n_ for n_ in nl if n_ in nr]; cl=[nl.index(n_) for n_ in com]; cr=[nr.index(n_) for n_ in com]
        sol=lsq_linear(np.vstack([Al[:,cl],Ar[:,cr],lam*np.eye(len(com))]),np.concatenate([bl,br,np.zeros(len(com))]),bounds=(0,1)).x
        for side,ti,cc in (('l',tl,cl),('r',tr,cr)):
            co=co+np.tensordot(sol,DD[[ti[c] for c in cc]],1)
            eye_info[side]={side+'-'+com[k]:float(v) for k,v in enumerate(sol) if v>0.02}
    else:
        for side,pre in (('l','l_'),('r','r_')):
            ti,A,b=eq(side,pre)
            sol=lsq_linear(np.vstack([A,lam*np.eye(len(ti))]),np.concatenate([b,np.zeros(len(ti))]),bounds=(0,1)).x
            co=co+np.tensordot(sol,DD[ti],1)
            eye_info[side]={tn[ti[k]]:float(v) for k,v in enumerate(sol) if v>0.02}
    for side,pre,mask in (('l','l_',m['leye']),('r','r_',m['reye'])):
        idx=[i for i,n_ in enumerate(ln) if n_.startswith(pre)]
        t_e=np.mean([tgt0[i]-co[src0[i]] for i in idx],0)
        c_=co[mask].mean(0); r_=np.median(np.linalg.norm(co[mask]-c_,axis=1)); d_=np.linalg.norm(co-c_,axis=1)
        wb=np.clip((r_+sched.get('blk_out',0.014)-d_)/(sched.get('blk_out',0.014)-sched.get('blk_in',0.006)),0,1)
        co=co+wb[:,None]*t_e
        eye_info[side+'_shift_mm']=(1000*t_e).round(1).tolist()
        lres=np.linalg.norm([tgt0[i]-co[src0[i]] for i in idx],axis=1)
        eye_info[side+'_lid_mm']=(1000*lres).round(1).tolist()
    print('EYEBLOCK',json.dumps(eye_info))
P=lambda x: (x-eye)*100.0                      # metres -> cm around the eye
co_c=P(co); rv=P(r['v'].astype(np.float64)); rn=r['n'].astype(np.float64)
skin=g['body']
head=skin&(co[:,2]>eye[2]-0.17)
def vn0(p):
    fn=np.cross(p[tris[:,1]]-p[tris[:,0]],p[tris[:,2]]-p[tris[:,0]]); vn=np.zeros_like(p)
    for k in range(3): np.add.at(vn,tris[:,k],fn)
    return vn/(np.linalg.norm(vn,axis=1,keepdims=True)+1e-12)
def inside(idx):
    t=tris[head[tris].all(1)]; A0=co[t[:,0]]; E1=co[t[:,1]]-A0; E2=co[t[:,2]]-A0
    nr=vn0(co); hit=np.zeros(len(idx),bool)
    for s0 in range(0,len(idx),64):
        ii=idx[s0:s0+64]; O=co[ii][:,None,:]+nr[ii][:,None,:]*1e-4; Dd=nr[ii][:,None,:]
        P_=np.cross(Dd,E2[None]); det=np.einsum('ijk,jk->ij',P_,E1)
        inv=1/np.where(np.abs(det)<1e-12,1e-12,det); T=O-A0[None]
        u=np.einsum('ijk,ijk->ij',T,P_)*inv; Q=np.cross(T,E1[None]); v=np.einsum('ijk,ijk->ij',Dd,Q)*inv
        tt=np.einsum('jk,ijk->ij',E2,Q)*inv
        own=(t[None,:,:]==ii[:,None,None]).any(2)
        hit[s0:s0+64]=((u>=0)&(v>=0)&(u+v<=1)&(tt>1e-4)&(tt<0.04)&~own&(np.abs(det)>1e-12)).any(1)
    return hit
mb=head&(np.abs(co[:,0]-eye[0])<0.035)&(co[:,2]>eye[2]-0.12)&(co[:,2]<eye[2]-0.015)&(co[:,1]>eye[1])&(co[:,1]<eye[1]+0.10)
hc=eye+np.array([0,0.07,-0.03])    # a point inside the head
n0=vn0(co); outward=np.einsum('ij,ij->i',n0,(co-hc)/np.linalg.norm(co-hc,axis=1,keepdims=True))
vest=head&(outward<-0.1)&(np.abs(co[:,0]-eye[0])<0.045)&(co[:,2]>eye[2]-0.12)&(co[:,2]<eye[2]-0.03)
interior=mb|vest                    # the mouth bag behind the lips and the vestibule in front of the gums
print('VESTIBULE',int((vest&~mb).sum()))
lc0=co[m['leye']].mean(0); rc0=co[m['reye']].mean(0); er0=np.median(np.linalg.norm(co[m['leye']]-lc0,axis=1))
socket=head&(((np.linalg.norm(co-lc0,axis=1)<er0+0.0008)&(co[:,1]>lc0[1]-0.6*er0))|((np.linalg.norm(co-rc0,axis=1)<er0+0.0008)&(co[:,1]>rc0[1]-0.6*er0)))
# the socket lining: near the ball, facing it
for c_ in (lc0,rc0):
    dd_=co-c_; dn_=np.linalg.norm(dd_,axis=1)
    lining=head&(dn_<er0+0.006)&(np.einsum('ij,ij->i',n0,-dd_/dn_[:,None])>0.3)
    socket|=lining
interior|=socket
print('INTERIOR',int(interior.sum()),'socket',int(socket.sum()))
free=head&~interior
if sched.get('eye_block'):
    core=np.zeros(len(co),bool)
    for c_ in (co[m['leye']].mean(0),co[m['reye']].mean(0)):
        core|=np.linalg.norm(co-c_,axis=1)<er0+sched.get('blk_in',0.006)
    free&=~core; interior|=core&head
    print('CORE',int(core.sum()))
# --- landmarks: pixel -> world point on each mesh
def pix2world(view,u,v):
    vw=V['views'][view]; loc=np.array(vw['loc']); rt=np.array(vw['right'])
    p=loc+rt*(u-N/2)*S; p[2]=loc[2]-(v-N/2)*S; return p
def pick(pts,nrm_side,view,u,v,rad):
    w=pix2world(view,u,v)
    if view=='front': a,b,depth=0,2,1; sign=1           # smallest y is front
    else: a,b,depth=1,2,0; sign=-1                      # largest x is the left side
    d2=np.hypot(pts[:,a]-w[a],pts[:,b]-w[b]); c=np.nonzero(d2<rad)[0]
    if len(c)==0: raise SystemExit(f'no surface under {view} {u},{v}')
    if rad<0.001: return c[np.argmin(sign*pts[c,depth])]
    f=sign*pts[c,depth]; c=c[f<f.min()+0.003]             # the front layer, then the nearest across
    return c[np.argmin(d2[c])]
skin_idx=np.nonzero(free)[0]
src_l=[]; tgt_l=[]; names=[]
for view,items in ((('front',LM['front']),('profile',LM['profile'])) if LF is None else ()):
    for name,(rp,mp) in items.items():
        mi=skin_idx[pick(co[skin_idx],None,view,*mp,0.003)]
        ri=pick(r['v'],None,view,*rp,0.0003)
        src_l.append(mi); tgt_l.append(rv[ri]); names.append(name)
        if view=='profile':                              # the right ear: the mirrored ray from -x
            cm=co[skin_idx].copy(); cm[:,0]*=-1; mi2=skin_idx[pick(cm,None,view,*mp,0.003)]
            rm=r['v'].copy(); rm[:,0]*=-1; ri2=pick(rm,None,view,*rp,0.0003)
            src_l.append(mi2); tgt_l.append(rv[ri2]); names.append(name+'_r')
# midline features from the profile curves at x=0
def midline(pts,isref):
    sel=(np.abs(pts[:,0])<(0.03 if isref else 0.25))&(True if isref else free); p=pts[sel]; idx=np.nonzero(sel)[0]
    out={}
    def front_at(z0,z1,most_front=True):
        s=(p[:,2]>z0)&(p[:,2]<z1)&(p[:,1]<1.0)
        zs=np.linspace(z0,z1,60); best=None
        prof=[]
        for z in zs:
            k=np.nonzero(s&(np.abs(p[:,2]-z)<0.06))[0]
            if len(k): j=k[np.argmin(p[k,1])]; prof.append((p[j,1],idx[j]))
        prof=np.array(prof)
        j=np.argmin(prof[:,0]) if most_front else np.argmax(prof[:,0])
        return int(prof[j,1])
    out['pronasale']=front_at(-7,-2)
    zp=pts[out['pronasale'],2]
    out['subnasale']=front_at(zp-2.5,zp-0.3,False)
    out['nasion']=front_at(-1.5,1.5,False)
    out['glabella']=front_at(pts[out['nasion'],2]+0.3,4)
    return out
mid_m,mid_r=(midline(co_c,False),midline(rv,True)) if LF is None else ({},{})
for k in mid_m: src_l.append(mid_m[k]); tgt_l.append(rv[mid_r[k]]); names.append(k)
src_l=np.array(src_l); tgt_l=np.array(tgt_l)
if LF is not None:
    names=list(LF['names']); src_l=LF['src'].copy(); tgt_l=P(LF['tgt'])
    if sched.get('eye_block'):
        keep=np.array([not n_.startswith(('l_','r_')) for n_ in names])
        names=[n_ for n_,k_ in zip(names,keep) if k_]; src_l=src_l[keep]; tgt_l=tgt_l[keep]
pass
lm_err0=np.linalg.norm(co_c[src_l]-tgt_l,axis=1)
for n_,e_,a_,b_ in zip(names,lm_err0,co_c[src_l],tgt_l): print('  LM %-14s %5.2f cm  mpfb %s ref %s'%(n_,e_,a_.round(2),b_.round(2)))
print('LM',len(names),'start error cm: median %.2f max %.2f (%s)'%(np.median(lm_err0),lm_err0.max(),names[int(np.argmax(lm_err0))]))
# --- free vertices: head skin; data weights by region (front frame pixels)
fi=np.nonzero(free)[0]; nf=len(fi); pos=-np.ones(len(co),np.int64); pos[fi]=np.arange(nf)
def frontpix(x):
    vw=V['views']['front']; loc=np.array(vw['loc'])
    return (x[:,0]-loc[0])/S+N/2, (loc[2]-x[:,2])/S+N/2
U,Vv=frontpix(co)
def box(u0,u1,v0,v1,f=8): 
    return np.clip(np.minimum.reduce([(U-u0)/f,(u1-U)/f,(Vv-v0)/f,(v1-Vv)/f]),0,1)
w=np.zeros(len(co))
# the face oval (no hair), the neck front
cx,cy,ax,ay=600,640,350,470
w=np.clip((1-np.hypot((U-cx)/ax,(Vv-cy)/ay))*12,0,1)*(co[:,1]<eye[1]+0.03)
w=np.maximum(w,box(380,820,1000,1200)*(co[:,1]<eye[1]+0.06))
ref_ok=np.ones(len(rv),bool)         # points of the reference the skin may be pulled to
if sched.get('bald'):
    # A bald clean-shaven reference: no hair to mask, and all of its skin is data: the skull, the cheeks back to the
    # ears, the jaw and its angles, under the chin, the neck down to the collar. Its cloth is not, and neither is the
    # rim where reg_export.py cut it. With the face oval alone the head came out 15 mm narrower at the jaw.
    w=np.ones(len(co))
    q_=r['v'].astype(np.float64)-eye
    if 'c' in r.files:               # cloth by colour: nearer the low sides of the model than its face, in one piece from below
        c_=r['c'].astype(np.float64); f2=np.c_[np.log((c_[:,1]+4)/(c_[:,0]+4)),np.log((c_[:,2]+4)/(c_[:,0]+4))]
        fc=(np.abs(q_[:,0])<0.055)&(q_[:,2]>-0.055)&(q_[:,2]<0.045)&(q_[:,1]<0.025); lo=(q_[:,2]<-0.165)&(np.abs(q_[:,0])>0.06)
        if lo.sum()>2000:
            ms_,mc_=np.median(f2[fc],0),np.median(f2[lo],0); ax_=mc_-ms_
            if np.linalg.norm(ax_)>0.08:
                like=((f2-ms_)@ax_)/(ax_@ax_)>0.5
                g_=0.004; o_=q_.min(0); vi=((q_-o_)/g_).astype(int); dims=vi.max(0)+1; flat=np.ravel_multi_index(vi.T,dims)
                frac=np.bincount(flat,like,minlength=dims.prod())/np.maximum(np.bincount(flat,minlength=dims.prod()),1)
                vox=(frac>0.6).reshape(dims); zc=o_[2]+(np.arange(dims[2])+0.5)*g_; vox[:,:,zc>-0.06]=False
                lab,nl=ndimage.label(vox); low=np.unique(lab[:,:,zc<-0.18]); low=low[low>0]
                cloth=np.isin(lab,low)[vi[:,0],vi[:,1],vi[:,2]]; ref_ok&=~cloth
                ang=np.degrees(np.arctan2(q_[:,0],-(q_[:,1]-0.07)))
                tops=[1000*np.percentile(q_[cloth&(ang>=a0)&(ang<a0+60),2],99) if (cloth&(ang>=a0)&(ang<a0+60)).sum()>50 else np.nan for a0 in range(-180,180,60)]
                print('CLOTH',int(cloth.sum()),'points of the reference; its top under the eyes by direction round the neck, mm:',np.round(tops,0))
            else: print('CLOTH none: the low sides of the reference have the colour of its face')
    pj=os.path.join(D,'place.json')
    if os.path.exists(pj):           # the head box of reg_export.py, where place_ref.py left it: its planes are not skin
        pl=json.load(open(pj)); s_=pl['scale']; t_=np.array(pl['shift_mm'])/1000
        cutp=(np.abs(q_[:,0]-t_[0])>0.12*s_-0.004)|(q_[:,2]<-0.20*s_+t_[2]+0.004)|(q_[:,1]>0.20*s_+t_[1]-0.004)
        ref_ok&=~cutp; print('CUT RIM',int(cutp.sum()),'points of the reference')
else:
    w*=1-box(430,770,695,792)          # moustache
    w*=1-box(400,800,858,1110)         # goatee
    w*=1-box(405,470,760,900)*1; w*=1-box(730,795,760,900)
    w*=1-box(-1e9,1e9,-1e9,215,20)      # above the hairline
w*=1-0.7*box(300,560,350,425); w*=1-0.7*box(640,900,350,425)   # brows are hair on the reference: weaker
# around the eyeballs: the lids follow the landmarks, not the reference's eyes
lc=co[m['leye']].mean(0); rc=co[m['reye']].mean(0)
er=np.median(np.linalg.norm(co[m['leye']]-lc,axis=1))
de=np.minimum(np.linalg.norm(co-lc,axis=1),np.linalg.norm(co-rc,axis=1))
w*=np.clip((de-(er+0.003))/0.002,0,1)
# ears from the side: where the skin faces sideways and lies in the ear box (profile pixels)
pv=V['views']['profile']; ploc=np.array(pv['loc'])
PU=(co[:,1]-ploc[1])/S+N/2; PV=(ploc[2]-co[:,2])/S+N/2
ear=np.clip(np.minimum.reduce([(PU-780)/10,(1060-PU)/10,(PV-360)/10,(730-PV)/10]),0,1)*(np.abs(co[:,0])>0.055)
w=np.maximum(w,ear)
cav=np.zeros(len(co),bool); hidx=np.nonzero(head&(w>0))[0]; cav[hidx]=inside(hidx)
w*=~cav; w*=np.clip((outward-0.1)/0.2,0,1)                            # nostrils, the ear canal, folds: their normal ray meets the head again
print('CAVITY',int(cav.sum()))
if sched.get('eye_block'):
    for c_ in (co[m['leye']].mean(0),co[m['reye']].mean(0)):
        w*=np.clip((np.linalg.norm(co-c_,axis=1)-(er0+sched.get('blk_in',0.006)))/sched.get('blk_fade',0.008),0,1)
w*=free
print('FREE',nf,'data-weighted',int((w>0.5).sum()))
# --- Amberg system
fe=edges[free[edges].any(1)&~interior[edges].any(1)]
ff=fe[free[fe].all(1)]; fx=fe[~free[fe].all(1)]
G=np.diag([1,1,1,1.0])
mc=eye+np.array([0,-0.025,-0.077])
near=lambda p,c,rr: np.linalg.norm(p-c,axis=1)<rr
ezone=(np.linalg.norm(co-co[m['leye']].mean(0),axis=1)<er0+0.006)|(np.linalg.norm(co-co[m['reye']].mean(0),axis=1)<er0+0.006)
lzone=(np.abs(co[:,0]-eye[0])<0.032)&(np.abs(co[:,2]-mc[2])<0.008)&(co[:,1]<eye[1])
zone=ezone|lzone
def ew(a,b): return sched.get('zone_w',4.0) if (zone[a] or zone[b]) else 1.0
def stiff(alpha):
    rows=[];cols=[];vals=[]
    for k,(a,b) in enumerate(ff):
        for d in range(4):
            q=alpha*ew(a,b)*G[d,d]; rows+= [4*k+d,4*k+d]; cols+=[4*pos[a]+d,4*pos[b]+d]; vals+=[q,-q]
    k0=len(ff); B=[]
    for k,(a,b) in enumerate(fx):
        i=a if free[a] else b
        for d in range(4): rows.append(4*(k0+k)+d); cols.append(4*pos[i]+d); vals.append(alpha*G[d,d])
    M=sparse.csr_matrix((vals,(rows,cols)),shape=(4*(len(ff)+len(fx)),4*nf))
    I4=np.vstack([np.eye(3),np.zeros((1,3))])
    Bs=np.zeros((4*(len(ff)+len(fx)),3))
    for k in range(len(fx)): Bs[4*(k0+k):4*(k0+k)+4]=alpha*(G@I4)
    return M,Bs
vh=np.hstack([co_c[fi],np.ones((nf,1))])
Dm=sparse.csr_matrix((vh.ravel(),(np.repeat(np.arange(nf),4),(4*np.arange(nf)[:,None]+np.arange(4)).ravel())),shape=(nf,4*nf))
lf=pos[src_l]; ok=lf>=0
lid=np.array([n_.startswith(('r_up','r_lo','l_up','l_lo','r_eye','l_eye')) for n_ in names])
lw=np.where(lid,sched.get('lid_w',1.0),1.0)[ok]
Dl=sparse.diags(lw)@Dm[lf[ok]]; Ul=tgt_l[ok]*lw[:,None]
tree=cKDTree(rv); print('TREE built')
X=np.tile(np.vstack([np.eye(3),np.zeros((1,3))]),(nf,1)); X0=X.copy(); eps=1e-3
def vnormals(p):
    fn=np.cross(p[tris[:,1]]-p[tris[:,0]],p[tris[:,2]]-p[tris[:,0]]); vn=np.zeros_like(p)
    for k in range(3): np.add.at(vn,tris[:,k],fn)
    return vn/(np.linalg.norm(vn,axis=1,keepdims=True)+1e-12)
cur=co_c.copy(); log=[]
wf=w[fi]
for alpha,beta in zip(sched['alpha'],sched['beta']):
    M,Bs=stiff(alpha)
    for it in range(sched['iters']):
        cur[fi]=Dm@X
        nrm=vnormals(cur)[fi]
        dist,j=tree.query(cur[fi]); U_=rv[j]
        cosn=np.abs(np.einsum('ij,ij->i',nrm,rn[j]))        # a generator winds its faces both ways: the sign of its normal means nothing
        wd=wf*(dist<sched['dmax'])*(cosn>sched['cos'])*ref_ok[j]
        Wd=sparse.diags(wd)
        A=sparse.vstack([M,Wd@Dm,beta*Dl,eps*sparse.identity(4*nf)]).tocsc(); Bm=np.vstack([Bs,wd[:,None]*U_,beta*Ul,eps*X0])
        At=A.T.tocsc(); lu=splu((At@A).tocsc()); Xn=lu.solve(At@Bm)
        dX=np.linalg.norm(Xn-X); X=Xn
        if dX<1e-3*np.sqrt(nf): break
    cur[fi]=Dm@X
    dist,_=tree.query(cur[fi]); wm=wf>0.5
    le_=np.linalg.norm(cur[src_l]-tgt_l,axis=1)
    lidr=np.linalg.norm(cur[src_l]-tgt_l,axis=1)[lid]
    rec={'lid_mm_median':float(10*np.median(lidr)),'alpha':alpha,'beta':beta,'iters':it+1,'skin_mm_median':float(10*np.median(dist[wm])),'skin_mm_p95':float(10*np.percentile(dist[wm],95)),'lm_mm_median':float(10*np.median(le_)),'lm_mm_max':float(10*le_.max())}
    log.append(rec); print('STEP',rec)
# helpers (not the eyes): follow the nearest skin vertices
new=co_c.copy(); new[fi]=cur[fi]
disp=new-co_c
blk=core if sched.get('eye_block') else np.zeros(len(co),bool)
if sched.get('eye_block'):                       # helpers and joints inside the block already moved with it
    for c_ in (co[m['leye']].mean(0),co[m['reye']].mean(0)): blk|=np.linalg.norm(co-c_,axis=1)<er0+sched.get('blk_in',0.006)
hk=np.nonzero(((~skin)|interior)&~socket&~blk&~m['leye']&~m['reye']&~g['jl']&~g['jr'])[0]
if len(hk):
    st=cKDTree(co_c[fi]); dd,kk=st.query(co_c[hk],k=32); ww=1/(dd+0.05)**4; ww/=ww.sum(1,keepdims=True)
    disp[hk]=(disp[fi][kk]*ww[:,:,None]).sum(1)
# eyeballs: shift each so the lid ring keeps its gap to the ball
eyes={}
for nm,mask,cen in (('l',m['leye'],lc),('r',m['reye'],rc)):
    c0=P(cen); rad=100*er
    d0=np.linalg.norm(co_c-c0,axis=1); ring=np.nonzero(free&(d0<rad+0.25)&(co_c[:,1]<c0[1]-0.5*rad))[0]
    g0=np.median(d0[ring]) if len(ring) else 0.0
    c=c0+(0 if sched.get('eye_block') else disp[ring].mean(0))
    for _ in range(0 if sched.get('eye_block') else 50):
        q=co_c[ring]+disp[ring]; dv=q-c; dn=np.linalg.norm(dv,axis=1)
        c=c+((dn-g0)[:,None]*dv/dn[:,None]).mean(0)*0.8
    sh=c-c0; disp[mask]=sh; disp[g['j'+nm]]=sh
    sk=socket&(np.linalg.norm(co_c-c0,axis=1)<rad+0.2); disp[sk]=sh; eyes[nm]=sh/100
    q=co_c[ring]+disp[ring]; gap=np.linalg.norm(q-c,axis=1)-rad if len(ring) else np.zeros(1)
    lv=np.nonzero(skin&~socket&(d0<rad+0.6))[0]
    q=co_c[lv]+disp[lv]; dv=q-c; dn=np.linalg.norm(dv,axis=1); out=dn<rad+0.03
    disp[lv[out]]+=(dv[out]/dn[out,None])*(rad+0.03-dn[out,None])
    allv=lv; ga=np.linalg.norm(co_c[allv]+disp[allv]-c,axis=1)-rad
    print('EYE',nm,'skin inside the ball after the shift',int((ga<0).sum()),'deepest mm %.2f'%(10*ga.min()))
    print('EYE',nm,'shift mm',(10*sh).round(1),'ring',len(ring),'gap mm min %.2f median %.2f'%(10*gap.min(),10*np.median(gap)))
newpos=co+disp/100.0
follow=np.zeros(len(co),bool); follow[hk]=True      # free, follow, ref_ok: for the pass after this one (head_outline.py)
np.savez(os.path.join(D,'reg.npz'),co=newpos,interior=interior,socket=socket,cav=cav,eye_l=eyes['l'],eye_r=eyes['r'],w=w,src_l=src_l,tgt_l=tgt_l/100+eye,
         free=free,follow=follow,ref_ok=ref_ok)
json.dump({'eye':eye_info,'sched':sched,'steps':log,'landmarks':names},open(os.path.join(D,'reg_log.json'),'w'),indent=1)
