#!/usr/bin/env python3
"""Report table for one machine: ordered rows, passes averaged, PSNR against the baseline's shots.
Usage: final_table.py ryzen|mac <rows: label=caption,...> <survey dir> [survey dir ...]"""
import json, re, statistics, subprocess, sys
from pathlib import Path
machine = sys.argv[1]
rows = [x.split("=", 1) for x in sys.argv[2].split(",")]
dirs = [Path(d) for d in sys.argv[3:]]
runs = {}
for d in dirs:
    for r in json.loads((d / "summary.json").read_text()):
        r["_dir"] = d / r["label"]
        runs.setdefault(r["label"], []).append(r)
def vals(rs, key, sub=None, spots=("city", "farm")):
    out = []
    for r in rs:
        for sp in spots:
            s = r.get("spots", {}).get(sp)
            if not s: continue
            x = s.get(sub, {}).get(key) if sub else s.get(key)
            if x is not None: out.append(x)
    return out
m = lambda v: statistics.fmean(v) if v else None
def psnr(a, b):
    if not (a.exists() and b.exists()): return None
    e = subprocess.run(["ffmpeg", "-hide_banner", "-i", str(a), "-i", str(b), "-lavfi", "psnr", "-f", "null", "-"], capture_output=True, text=True).stderr
    g = re.search(r"average:([0-9.inf]+)", e)
    return None if not g else float("inf") if g.group(1) == "inf" else float(g.group(1))
fmt = lambda v, f="{:.0f}": "—" if v is None else ("∞" if v == float("inf") else f.format(v).replace(".", ",").replace("-", "−"))
memk = "mem_used_mib" if machine == "ryzen" else "in_use_mem"
utk = "util_gpu" if machine == "ryzen" else "device_util"
msk = "gpu_ms_maxclk" if machine == "ryzen" else "gpu_ms_est"
base = runs["default"]
bmem = m(vals(base, memk, "gpu"))
bdir = base[0]["_dir"]
if machine == "ryzen":
    print("| Настройка | FPS город / ферма | GPU, % | ≈GPU, мс на 2100 МГц, город / ферма | МГц | Вт | Δ VRAM | PSNR к default: город днём / ночью |")
    print("|---|---|---|---|---|---|---|---|")
else:
    print("| Настройка | Прогонов | FPS город / ферма | GPU, % | ≈GPU, мс, город / ферма | Δ память GPU | PSNR к default: город днём / ночью |")
    print("|---|---|---|---|---|---|---|")
for label, cap in rows:
    rs = runs.get(label)
    if not rs: continue
    fc, ff = m(vals(rs, "fps", spots=("city",))), m(vals(rs, "fps", spots=("farm",)))
    mc, mf = m(vals(rs, msk, spots=("city",))), m(vals(rs, msk, spots=("farm",)))
    mem = m(vals(rs, memk, "gpu"))
    dm = None if mem is None else (mem - bmem) if machine == "ryzen" else (mem - bmem) / 2**30
    d = rs[0]["_dir"]
    p1, p2 = psnr(bdir / "city.png", d / "city.png"), psnr(bdir / "city_night.png", d / "city_night.png")
    if machine == "ryzen":
        print(f"| {cap} | {fmt(fc)} / {fmt(ff)} | {fmt(m(vals(rs, utk, 'gpu')))} | {fmt(mc,'{:.1f}')} / {fmt(mf,'{:.1f}')} | {fmt(m(vals(rs,'clock_gr','gpu')))} | "
              f"{fmt(m(vals(rs,'power_w','gpu')))} | {fmt(dm,'{:+.0f} МБ')} | {fmt(p1,'{:.0f}')} / {fmt(p2,'{:.0f}')} |")
    else:
        print(f"| {cap} | {len(rs)} | {fmt(fc)} / {fmt(ff)} | {fmt(m(vals(rs, utk, 'gpu')))} | {fmt(mc,'{:.1f}')} / {fmt(mf,'{:.1f}')} | "
              f"{fmt(dm,'{:+.2f} ГБ')} | {fmt(p1,'{:.0f}')} / {fmt(p2,'{:.0f}')} |")
