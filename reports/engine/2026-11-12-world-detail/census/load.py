"""Load all chunks of a world from the base into a compact npz (read-only on the base)."""
import os, sys, json, pickle, time
from pathlib import Path
import numpy as np
REPO=str(Path(__file__).resolve().parents[4]/'scripts'/'content'/'world')
sys.path.insert(0, REPO)
import chunk_glb as cg
BASE=Path(os.environ.get('OPENGOTHIC_BASE') or max((p for p in (Path.home()/'.cache'/'gothic-ai'/'base').iterdir() if (p/'base.json').exists()),key=lambda p:(p/'base.json').stat().st_mtime))/'worlds'
world=sys.argv[1]
mats=json.load(open(BASE/world/'world.json'))['materials']
byname={m['name']:i for i,m in enumerate(mats)}
P=[];UV=[];M=[];C=[]
t=time.time()
for f in sorted((BASE/world/'chunks').glob('*.glb')):
    ch=cg.read_chunk(f)
    x,z=cg.chunk_coords(f.stem)
    for p in ch.primitives:
        n=len(p.positions)
        P.append(p.positions); UV.append(p.uvs)
        M.append(np.full(n, byname.get(p.material.get('name'),-1), np.int32))
        C.append(np.full((n,2),(x,z),np.int32))
P=np.concatenate(P);UV=np.concatenate(UV);M=np.concatenate(M);C=np.concatenate(C)
np.savez(f'{world}.npz',P=P,UV=UV,M=M,C=C)
print(world,len(P),'tris',time.time()-t,'s','unmapped',(M<0).sum())
