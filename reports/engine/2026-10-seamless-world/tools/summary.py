import sys,glob,os
# usage: summary.py <OUT dir of runs.sh> > summary.csv
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from parse import parse, S
def segs(rows):
    # split into sessions/changes: each segment ends with session_end or change_end
    out=[];cur=[]
    for r in rows:
        cur.append(r)
        if r['stage'] in('session_end','change_end'): out.append(cur);cur=[]
    return out
def get(seg,st,k='stage_ms'):
    v=[r[k] for r in seg if r['stage']==st]
    return v[-1] if v else 0
def delta(seg,st,k):
    for i,r in enumerate(seg):
        if r['stage']==st and i>0: return r[k]-seg[i-1][k]
    return 0
hdr="run,seg,world,total_s,vm,data,chunks,pack,view,physics,cloth,vobs,movable,waynet_idx,dialogs_player,startup,store_old,restore,tex_n,tex_mb,tex_s,fp_end,fp_peak,gpu_end,npcs,items,d_data,d_chunks,d_pack,d_view,d_gpu_view,d_phys,d_vobs,d_gpu_vobs,d_worldend,d_startup,d_gpu_startup"
print(hdr)
for run in sorted(glob.glob(S+'/runs/*/game.log')):
    name=run.split('/')[-2]
    for i,seg in enumerate(segs(parse(run))):
        w=[r.get('world') for r in seg if r['stage']=='world_begin']
        t0=seg[0]['t_ms']; t1=seg[-1]['t_ms']
        vals=[name,i,(w[0] if w else '?').replace('.ZEN',''),round((t1-t0)/1000,2)]
        for st in['scripts_vm','world_data_read','mesh_chunks_read','landscape_pack','landscape_view','physics_static','cloth_surfaces','vobs','movable_vobs','waynet_index']:
            vals.append(round(get(seg,st)))
        vals.append(round(get(seg,'dialogs')+get(seg,'player_postinit')))
        vals.append(round(get(seg,'scripts_startup')))
        vals.append(round(get(seg,'change_store_old'))); vals.append(round(get(seg,'change_restore_state')))
        e=seg[-1]; b=seg[0]
        vals+= [int(e['tex_n']-b['tex_n']) if i else int(e['tex_n']), round(e['tex_mb']-(b['tex_mb'] if i else 0)), round((e['tex_ms']-(b['tex_ms'] if i else 0))/1000,2)]
        vals+= [round(e['footprint_mb']), round(max(r['footprint_mb'] for r in seg)), round(e['gpu_mb'])]
        vals+= [int(get(seg,'scripts_startup','npcs')), int(get(seg,'vobs','items'))]
        vals+= [round(delta(seg,s,k)) for s,k in [('world_data_read','footprint_mb'),('mesh_chunks_read','footprint_mb'),('landscape_pack','footprint_mb'),('landscape_view','footprint_mb'),('landscape_view','gpu_mb'),('physics_static','footprint_mb'),('vobs','footprint_mb'),('vobs','gpu_mb'),('world_end','footprint_mb'),('scripts_startup','footprint_mb'),('scripts_startup','gpu_mb')]]
        print(','.join(map(str,vals)))
