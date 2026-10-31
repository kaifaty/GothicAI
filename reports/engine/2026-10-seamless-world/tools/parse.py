import re,sys,glob,os,json
# usage: parse.py <OUT dir of runs.sh>  (reads <OUT>/runs/*/game.log)
S=sys.argv[1] if len(sys.argv)>1 else '.'
def parse(path):
    rows=[]
    for line in open(path,errors='replace'):
        m=re.search(r'\[loadprobe\] stage=(\S+) (.*)',line)
        if not m: continue
        d={'stage':m.group(1)}
        for k,v in re.findall(r'(\w+)=(\S+)',m.group(2)):
            try: d[k]=float(v)
            except: d[k]=v
        rows.append(d)
    return rows
STAGES=['scripts_vm','world_data_read','mesh_chunks_read','landscape_pack','landscape_view','physics_static','cloth_surfaces','waynet','vobs','movable_vobs','waynet_index','world_end','dialogs','player_postinit','scripts_startup']
def main():
  for run in sorted(glob.glob(S+'/runs/*/game.log')):
    name=run.split('/')[-2]
    rows=parse(run)
    if not rows: print(name,'NO DATA'); continue
    print('==',name)
    prev=None
    for r in rows:
        st=r['stage']
        dfp = r['footprint_mb']-prev['footprint_mb'] if prev else 0
        dgpu= r['gpu_mb']-prev['gpu_mb'] if prev else 0
        dtex= r['tex_mb']-prev['tex_mb'] if prev else 0
        dtms= r['tex_ms']-prev['tex_ms'] if prev else 0
        extra=' '.join(f'{k}={int(v) if isinstance(v,float) else v}' for k,v in r.items() if k not in('stage','stage_ms','t_ms','rss_mb','footprint_mb','gpu_mb','tex_n','tex_mb','tex_ms'))
        print(f"  {st:22s} {r['stage_ms']:8.0f} ms  t={r['t_ms']:8.0f}  rss={r['rss_mb']:6.0f} fp={r['footprint_mb']:6.0f}({dfp:+5.0f}) gpu={r['gpu_mb']:5.0f}({dgpu:+5.0f}) tex={r['tex_mb']:5.0f}({dtex:+4.0f}MB,{dtms:+5.0f}ms) {extra}")
        prev=r

if __name__=='__main__': main()
