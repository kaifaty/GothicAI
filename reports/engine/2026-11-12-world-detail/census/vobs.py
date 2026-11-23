import os, json, collections, re, sys
from pathlib import Path
import numpy as np
REPO=str(Path(__file__).resolve().parents[4]/'scripts'/'content'/'world')
sys.path.insert(0,REPO); import chunk_glb as cg
B=Path(os.environ.get('OPENGOTHIC_BASE') or max((p for p in (Path.home()/'.cache'/'gothic-ai'/'base').iterdir() if (p/'base.json').exists()),key=lambda p:(p/'base.json').stat().st_mtime))
world=sys.argv[1] if len(sys.argv)>1 else 'NEWWORLD'
v=json.load(open(B/'worlds'/world/'vobs.json'))
cnt=collections.Counter(); inst=collections.Counter(); cls=collections.Counter(); vt=collections.Counter()
def walk(l):
    for o in l:
        n=(o.get('visual_name') or '').upper()
        cnt[n]+=1; cls[o['class']]+=1; vt[o.get('associated_visual_type')]+=1
        walk(o.get('children') or [])
walk(v['vobs'])
def kind(n):
    s=re.sub(r'\.(3DS|MDL|MDS|ASC|MMS|MMB)$','',n)
    if n.endswith(('.TGA','.PFX','.WAV','.SLV','.')) or not n: return None
    if re.search(r'TREE|TANNE|BAUM(?!STAMM)|PINE|PALM',s) and 'TREEBARK' not in s: return 'trees'
    if 'BAUMSTAMM' in s or 'STUMP' in s or 'WOODLOG' in s: return 'trunks/logs'
    if re.search(r'BUSH|HECKE|LONG_BUSH',s): return 'bushes'
    if re.search(r'FARN|FERN|GRASS|PLANT|REED|SCHILF|FLOWER|HERB|MUSHROOM|WEED|MOSS|HANGPLANT|CACTUS|VINE|LIANE',s) or s.startswith(('OW_NATURE_SIDE','NW_NATURE_SIDE')): return 'plants/grass/ferns'
    if re.search(r'STONE_|STEIN|ROCK|FELS|BOULDER|COUPLESTONES|STONEBIG|_STONE$|ROCKS',s) and not re.search(r'STONEWALL|STONEMILL|STONEHENGE|PFLASTER',s): return 'rocks'
    return 'other static visuals'
by=collections.defaultdict(collections.Counter)
for n,c in cnt.items():
    k=kind(n)
    if k: by[k][n]+=c
def tris(n):
    s=re.sub(r'\.(3DS|MDL|ASC)$','',n); f=B/'meshes'/(s+'.glb')
    if not f.exists(): return None
    try:
        doc,bin_=cg.read_glb(f); t=0
        for m in doc.get('meshes',[]):
            for p in m['primitives']:
                t+=doc['accessors'][p['indices']]['count']//3 if 'indices' in p else doc['accessors'][p['attributes']['POSITION']]['count']//3
        return t
    except Exception as e: return None
res={'world':world,'total_vobs':sum(cnt.values()),'classes':dict(cls.most_common(12)),'visual_types':dict(vt.most_common(8))}
out=[f'## {world}: vobs (vobs.json, whole tree incl. children)\n',f"Total vob objects: {sum(cnt.values()):,}; with a mesh/model visual (names not .TGA/.PFX/.WAV): {sum(sum(c.values()) for c in by.values()):,}.\n"]
rows=[]
for k in ['trees','trunks/logs','bushes','plants/grass/ferns','rocks','other static visuals']:
    c=by[k]; tot=sum(c.values()); ts=[(n,m,tris(n)) for n,m in c.most_common()]
    wt=sum(m*t for n,m,t in ts if t is not None); wc=sum(m for n,m,t in ts if t is not None)
    rows.append((k,tot,len(c),wc,wt,ts))
out.append('| class | instances | distinct meshes | instances with readable mesh | triangles of those instances | mean tris/instance |\n|---|---|---|---|---|---|\n')
for k,tot,d,wc,wt,ts in rows: out.append(f'| {k} | {tot:,} | {d} | {wc:,} | {wt:,} | {wt/max(wc,1):.0f} |\n')
for k,tot,d,wc,wt,ts in rows:
    out.append(f'\n### Top meshes: {k}\n\n| visual | instances | mesh triangles |\n|---|---|---|\n')
    for n,m,t in ts[:12 if k!='other static visuals' else 15]: out.append(f'| {n} | {m} | {t if t is not None else "n/a"} |\n')
open(f'{world}_vobs.md','w').write(''.join(out)); json.dump(res,open(f'{world}_vobs.json','w'),indent=1)
print(''.join(out))
