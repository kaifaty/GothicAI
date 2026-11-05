# Blink for the registered head: MPFB's ARKit eyeBlink as the start, then the upper lid margin laid onto the lower
# one, the correction spread over the lid harmonically, and every lid vertex kept 0.3 mm outside the eyeball.
#   python blink_build.py <reg dir> <round dir with reg.npz, reg_log.json> <out blink.npz>
import sys, os, json, numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve
from scipy.spatial import cKDTree
D,RD,out=sys.argv[1:4]
m=np.load(os.path.join(D,'mpfb.npz')); g=np.load(os.path.join(D,'groups.npz')); E=np.load(os.path.join(D,'eye_deltas.npz'))
R=np.load(os.path.join(RD,'reg.npz')); log=json.load(open(os.path.join(RD,'reg_log.json'))); C=json.load(open(os.path.join(D,'lid_chains.json')))
neu=R['co']; n=len(neu); body=g['body']; edges=m['edges']
names=list(E['names']); GAP=0.0003
def arc(p):
    s=np.concatenate([[0],np.cumsum(np.linalg.norm(np.diff(p,axis=0),axis=1))]); return s/s[-1]
res={}; rep={}
for side,mask,sg in (('l',m['leye'],1),('r',m['reye'],-1)):
    Db=E['D'][names.index(f'{side}-fu-eyeBlink')].astype(np.float64)
    b0=log['eye'][side].get(f'{side}-fu-eyeBlink',0.0)
    closed=neu+(1-b0)*Db
    # the eyeball where the registration put it
    sel=(m['eye_obj_co'][:,0]*sg)>0; idx=np.nonzero(sel)[0]
    shift=R['co'][mask].mean(0)-m['co'][mask].mean(0)
    ev=m['eye_obj_co'][idx]+shift
    tri=m['eye_obj_tris']; tri=tri[np.isin(tri,idx).all(1)]; remap=-np.ones(len(m['eye_obj_co']),int); remap[idx]=np.arange(len(idx)); tri=remap[tri]
    fn=np.cross(ev[tri[:,1]]-ev[tri[:,0]],ev[tri[:,2]]-ev[tri[:,0]]); vn=np.zeros_like(ev)
    for k in range(3): np.add.at(vn,tri[:,k],fn)
    vn/=np.linalg.norm(vn,axis=1,keepdims=True)+1e-12
    cen=ev.mean(0)
    if np.mean(np.einsum('ij,ij->i',vn,ev-cen))<0: vn=-vn          # outward
    et=cKDTree(ev)
    # the lid region: what the blink moves, plus the lining around the ball
    reg_=body&((np.linalg.norm(Db,axis=1)>5e-5)|(np.linalg.norm(neu-cen,axis=1)<0.0145))
    up=np.array(C[side]['upper']); lo=np.array(C[side]['lower'])
    su=arc(closed[up]); sl=arc(closed[lo])
    tgt=np.stack([np.interp(su,sl,closed[lo][:,k]) for k in range(3)],1)
    tgt[:,2]+=0.00015                                              # the upper margin rests just on the lower one
    e_up=tgt-closed[up]
    # harmonic spread of the correction over the lid region; fixed: the upper margin (e), the lower margin and the
    # region's rim (0)
    ri=np.nonzero(reg_)[0]; pos=-np.ones(n,int); pos[ri]=np.arange(len(ri))
    fixed=np.zeros(n,bool); val=np.zeros((n,3))
    fixed[up]=True; val[up]=e_up; fixed[lo]=True
    ee=edges[reg_[edges].all(1)]
    rim=np.zeros(n,bool)
    for a,bb in edges:
        if reg_[a]!=reg_[bb]: rim[a if reg_[a] else bb]=True
    fixed|=rim&~np.isin(np.arange(n),up)
    free=reg_&~fixed; fi=np.nonzero(free)[0]; fp=-np.ones(n,int); fp[fi]=np.arange(len(fi))
    rows=[];cols=[];vals=[]; B=np.zeros((len(fi),3)); deg=np.zeros(len(fi))
    for a,bb in ee:
        for x,y in ((a,bb),(bb,a)):
            if free[x]:
                deg[fp[x]]+=1
                if free[y]: rows.append(fp[x]); cols.append(fp[y]); vals.append(-1.0)
                else: B[fp[x]]+=val[y]
    Lm=sparse.csr_matrix((vals,(rows,cols)),shape=(len(fi),len(fi)))+sparse.diags(deg)
    corr=val.copy(); corr[fi]=spsolve(Lm.tocsc(),B) if len(fi) else 0
    P=closed+corr
    # keep the lid outside the ball (and the cornea): push along the ball's normal, smooth, repeat
    lid=np.nonzero(reg_)[0]
    for it in range(6):
        d,j=et.query(P[lid]); s=np.einsum('ij,ij->i',P[lid]-ev[j],vn[j])
        bad=s<GAP; P[lid[bad]]+=vn[j[bad]]*(GAP-s[bad])[:,None]
        if it<5:   # a light smoothing of the push, margins held
            dp=P-closed-corr; acc=np.zeros_like(dp); cnt=np.zeros(n)
            np.add.at(acc,ee[:,0],dp[ee[:,1]]); np.add.at(acc,ee[:,1],dp[ee[:,0]]); np.add.at(cnt,ee[:,0],1); np.add.at(cnt,ee[:,1],1)
            sm=np.where(cnt[:,None]>0,acc/np.maximum(cnt,1)[:,None],0)
            mv=free&reg_; P[mv]=closed[mv]+corr[mv]+0.5*dp[mv]+0.5*sm[mv]
    d,j=et.query(P[lid]); s=np.einsum('ij,ij->i',P[lid]-ev[j],vn[j])
    gap_m=np.linalg.norm(P[up]-np.stack([np.interp(arc(P[up]),arc(P[lo]),P[lo][:,k]) for k in range(3)],1),axis=1)
    rep[side]={'b0':b0,'inside_ball':int((s<0).sum()),'min_clear_mm':round(1000*float(s.min()),2),'margin_gap_mm_max':round(1000*float(gap_m.max()),2),'margin_gap_mm_median':round(1000*float(np.median(gap_m)),2),'upper_margin_move_mm':round(1000*float(np.linalg.norm(e_up,axis=1).max()),2)}
    # in-between frames: a straight blend cuts the ball, so each frame is pushed out on its own and smoothed
    full=P-neu; frames=[]
    fn0=np.cross(neu[m['tris'][:,1]]-neu[m['tris'][:,0]],neu[m['tris'][:,2]]-neu[m['tris'][:,0]]); vv=np.zeros_like(neu)
    for k in range(3): np.add.at(vv,m['tris'][:,k],fn0)
    lining=reg_&(np.einsum('ij,ij->i',vv,neu-cen)<0)&(np.linalg.norm(neu-cen,axis=1)<np.linalg.norm(ev-cen,axis=1).max()+0.002)
    for c_ in (C[side]['upper'],C[side]['lower']): lining[c_]=False
    for tt_ in (0.25,0.5,0.75):
        Q=neu+tt_*full
        for it in range(6):
            d,j=et.query(Q[lid]); s_=np.einsum('ij,ij->i',Q[lid]-ev[j],vn[j]); bad=s_<GAP
            Q[lid[bad]]+=vn[j[bad]]*(GAP-s_[bad])[:,None]
            dq=Q-neu-tt_*full; acc=np.zeros_like(dq); cnt=np.zeros(n)
            np.add.at(acc,ee[:,0],dq[ee[:,1]]); np.add.at(acc,ee[:,1],dq[ee[:,0]]); np.add.at(cnt,ee[:,0],1); np.add.at(cnt,ee[:,1],1)
            sm=np.where(cnt[:,None]>0,acc/np.maximum(cnt,1)[:,None],0); mv=reg_
            Q[mv]=neu[mv]+tt_*full[mv]+0.5*dq[mv]+0.5*sm[mv]
        for _ in range(3):
            dq=Q-neu; acc=np.zeros_like(dq); cnt=np.zeros(n)
            np.add.at(acc,ee[:,0],dq[ee[:,1]]); np.add.at(acc,ee[:,1],dq[ee[:,0]]); np.add.at(cnt,ee[:,0],1); np.add.at(cnt,ee[:,1],1)
            Q[lining]=neu[lining]+acc[lining]/np.maximum(cnt[lining],1)[:,None]
        d,j=et.query(Q[lid]); s_=np.einsum('ij,ij->i',Q[lid]-ev[j],vn[j])
        rep[side]['t%.2f_min_clear_mm'%tt_]=round(1000*float(s_.min()),2)
        frames.append((Q-neu).astype(np.float32))
    res[side]=np.stack(frames+[full.astype(np.float32)])
print('BLINK',json.dumps(rep))
np.savez(out,l=res['l'],r=res['r'],t=np.array([0.25,0.5,0.75,1.0]))
