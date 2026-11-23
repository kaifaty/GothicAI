import json, glob, statistics as st, sys
T=sys.argv[1]
rows={}
for wt,tag in (('taa-fix-main','main'),('taa-fix-new','fix')):
    for d in sorted(glob.glob(f'{T}/{wt}/.bench/perf-*')):
        r=json.load(open(d+'/report.json'))
        for place,v in r['places'].items():
            s=v['summary']; gp=s['gpu_passes']
            x={'frame':s['gpu']['mean_ms']}
            for lab in ('Translucent','TAA','Reactive-clear'):
                if lab in gp: x[lab]=gp[lab]['mean_ms']
            rows.setdefault((place,tag),[]).append(x)
for place in ('harbour-night','market','forest'):
    print(place)
    for col in ('frame','Translucent','TAA','Reactive-clear'):
        a=[x.get(col,0) for x in rows[(place,'main')]]; b=[x.get(col,0) for x in rows[(place,'fix')]]
        print(f"  {col:15s} main {' / '.join(f'{v:.3f}' for v in a)}  fix {' / '.join(f'{v:.3f}' for v in b)}  diff {st.fmean(b)-st.fmean(a):+.3f}")
