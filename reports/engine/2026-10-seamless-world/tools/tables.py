# usage: tables.py <summary.csv> <local|ryzen>  -> markdown tables of the report
import csv,sys
rows=list(csv.DictReader(open(sys.argv[1])))
host=sys.argv[2]  # local|ryzen
H={'local':'Mac','ryzen':'Ryzen'}[host]
def f(v): 
    v=float(v); return f"{v:,.0f}".replace(',',' ')
def fs(v): return f"{float(v):.1f}".replace('.',',')
def R(name,seg='0'):
    for r in rows:
        if r['run']==name and r['seg']==seg: return r
W={'NEWWORLD':'Хоринис','OLDWORLD':'Долина','ADDONWORLD':'Яркендар'}
SET={'orig':'оригинал','work':'ремастер'}
out=[]
out.append(f"**{H}.**\n")
out.append("| Мир, набор | VM | данные мира | куски меша | упаковка | вид ландшафта | физика | ткань | vob'ы | индекс путей | диалоги, игрок | стартовые скрипты | **итого, с** |")
out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for w in ['NEWWORLD','OLDWORLD','ADDONWORLD']:
    for s in ['orig','work']:
        r=R(f'{host}-{s}-{w}-par')
        if not r: continue
        out.append(f"| {W[w]}, {SET[s]} | {f(r['vm'])} | {f(r['data'])} | {f(r['chunks'])} | {f(r['pack'])} | {f(r['view'])} | {f(r['physics'])} | {f(r['cloth'])} | {f(float(r['vobs'])+float(r['movable']))} | {f(r['waynet_idx'])} | {f(r['dialogs_player'])} | {f(r['startup'])} | **{fs(r['total_s'])}** |")
print('\n'.join(out)); print()
# memory serial
out=[f"**{H}**, прирост на стадии, МБ (footprint / {'Metal' if host=='local' else 'Vulkan'}):\n",
"| Мир, набор | данные мира | куски меша | упаковка | вид ландшафта | физика | vob'ы | освобождение меша | стартовые скрипты (NPC) | **конец** | **пик** |",
"|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
for s,w in [('orig','NEWWORLD'),('work','NEWWORLD'),('work','OLDWORLD'),('work','ADDONWORLD')]:
    r=R(f'{host}-{s}-{w}-serial')
    if not r: continue
    out.append(f"| {W[w]}, {SET[s]} | +{f(r['d_data'])} | +{f(r['d_chunks'])} | +{f(r['d_pack'])} | +{f(r['d_view'])} / +{f(r['d_gpu_view'])} | +{f(r['d_phys'])} | +{f(r['d_vobs'])} / +{f(r['d_gpu_vobs'])} | {f(r['d_worldend'])} | +{f(r['d_startup'])} / +{f(r['d_gpu_startup'])} | **{f(r['fp_end'])} / {f(r['gpu_end'])}** | {f(r['fp_peak'])} |")
print('\n'.join(out)); print()
out=["| Мир | набор | текстур | текстуры, МБ | загрузка текстур, с | footprint в конце, МБ | GPU в конце, МБ | итого загрузка, с |","|---|---|---:|---:|---:|---:|---:|---:|"]
for w in ['NEWWORLD','OLDWORLD','ADDONWORLD']:
    for s in ['orig','work']:
        r=R(f'{host}-{s}-{w}-par')
        if not r: continue
        out.append(f"| {W[w]} | {SET[s]} | {r['tex_n']} | {f(r['tex_mb'])} | {fs(r['tex_s'])} | {f(r['fp_end'])} | {f(r['gpu_end'])} | {fs(r['total_s'])} |")
print(f"**{H}.**\n"); print('\n'.join(out)); print()
segs=[r for r in rows if r['run']==f'{host}-work-NEWWORLD-chain']
if segs:
    out=["| Шаг | мир | загрузка, с | сохранение ушедшего, мс | восстановление, мс | стартовые скрипты, мс | новых текстур, МБ | footprint, МБ | GPU, МБ | NPC |","|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for r in segs:
        out.append(f"| {int(r['seg'])+1} | {W.get(r['world'].upper().replace('.ZEN',''),r['world'])} | {fs(r['total_s'])} | {f(r['store_old'])} | {f(r['restore'])} | {f(r['startup'])} | {f(r['tex_mb'])} | {f(r['fp_end'])} | {f(r['gpu_end'])} | {r['npcs']} |")
    print(f"**{H}.**\n"); print('\n'.join(out))
