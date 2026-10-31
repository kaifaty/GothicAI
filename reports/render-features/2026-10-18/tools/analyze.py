#!/usr/bin/env python3
"""Summarise one survey directory: per configuration and spot, presented frame rate, frame intervals,
CPU encode, the serial (capture) frame, GPU load from the sampler, VRAM, log errors.
Usage: analyze.py <survey dir> [spot ...]  ->  prints a table, writes summary.json"""
import csv, json, re, statistics, sys
from pathlib import Path

SUBMITTED, FENCE, CAPTURE = 1, 2, 3
CAPREQ = 4

def pct(v, p):
    if not v: return None
    v = sorted(v); k = (len(v) - 1) * p / 100
    lo = int(k); hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)

def load_frames(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows

def gpu_window(samples, t0, t1, cols):
    sel = [s for s in samples if t0 <= int(s["mono_ns"]) <= t1]
    out = {}
    for c in cols:
        vals = [float(s[c]) for s in sel if s.get(c) not in (None, "", "[N/A]")]
        out[c] = statistics.fmean(vals) if vals else None
    out["n"] = len(sel)
    return out

ERR = re.compile(r"(error|exception|assert|fail|validation|vk_error|crash|abort|segmentation|terminate)", re.I)
NOISE = re.compile(r"(unable to load|not found|unrecognized|nan|failed to read|no texture|sound|\[bench\] can not open)", re.I)

def analyze(d, spots):
    res = {"label": d.name, "flags": (d / "flags.txt").read_text().strip() if (d / "flags.txt").exists() else ""}
    res["exit"] = (d / "exit.txt").read_text().strip() if (d / "exit.txt").exists() else "?"
    log = (d / "game.log").read_text(errors="replace") if (d / "game.log").exists() else ""
    m = re.search(r"GPU = (.*)", log); res["gpu"] = m.group(1).strip() if m else None
    res["done"] = "[bench] done" in log
    errs = [l for l in log.splitlines() if ERR.search(l) and not NOISE.search(l)]
    res["errors"] = errs[:40]; res["n_errors"] = len(errs)
    shots = sorted(p.name for p in d.glob("*.png"))
    res["shots"] = shots
    fpath = d / "frames.csv"
    if not fpath.exists():
        return res
    rows = load_frames(fpath)
    res["attempts"] = len(rows); res["overflow"] = int(rows[-1]["overflow"]) if rows else 0
    capidx = [i for i, r in enumerate(rows) if int(r["flags"]) & CAPREQ]
    groups = []
    for i in capidx:
        if groups and i - groups[-1][-1] <= 2:
            groups[-1].append(i)
        else:
            groups.append([i])
    shots_i = [g[0] for g in groups if len(g) == 1]
    recs = [g for g in groups if len(g) > 1]
    res["n_shots"] = len(shots_i); res["n_recs"] = len(recs)
    gpu_cols, samples = [], []
    for name in ("gpu.csv",):
        if (d / name).exists():
            with open(d / name) as f:
                samples = list(csv.DictReader(f))
            gpu_cols = [c for c in (samples[0].keys() if samples else []) if c != "mono_ns" and c != "pstate"]
    idle = None
    if (d / "idle-gpu.csv").exists():
        with open(d / "idle-gpu.csv") as f:
            iv_ = [float(r["device_util"]) for r in csv.DictReader(f) if r.get("device_util")]
        idle = statistics.fmean(iv_) if iv_ else None
    res["idle_util"] = idle
    # markers come in pairs per spot: <spot>_a then <spot>
    res["spots"] = {}
    for k, spot in enumerate(spots):
        # three shots per spot: <spot>_a (window start), <spot> (window end), <spot>_night; one rec
        if 3 * k + 1 >= len(shots_i):
            break
        a, b = shots_i[3 * k], shots_i[3 * k + 1]
        win = rows[a + 1:b]
        sub = [r for r in win if int(r["outcome"]) == SUBMITTED]
        t0, t1 = int(rows[a]["end_ns"]), int(rows[b]["begin_ns"])
        wall = (t1 - t0) / 1e6
        begins = [int(r["begin_ns"]) for r in sub]
        iv = [(y - x) / 1e6 for x, y in zip(begins, begins[1:])]
        ms = lambda r, z: int(r[z + "_ns"]) / 1e6
        enc = [ms(r, "encode") for r in sub]
        pres = [ms(r, "present") for r in sub]
        sim = [ms(r, "tick") + ms(r, "finish_step") for r in win if int(r["dt"]) > 0]
        s = {"wall_ms": wall, "frames": len(sub), "fence_busy": sum(1 for r in win if int(r["outcome"]) == FENCE),
             "ticks": sum(1 for r in win if int(r["dt"]) > 0),
             "fps": len(sub) / (wall / 1000) if wall > 0 else None,
             "iv_med": pct(iv, 50), "iv_p95": pct(iv, 95), "iv_p99": pct(iv, 99),
             "encode_med": pct(enc, 50), "present_med": pct(pres, 50), "sim_med": pct(sim, 50)}
        if samples:
            g = gpu_window(samples, t0, t1, gpu_cols)
            s["gpu"] = g
            util = g.get("util_gpu", g.get("device_util"))
            if util is not None and s["frames"]:
                s["gpu_ms_est"] = util / 100 * wall / s["frames"]
                if idle is not None:
                    # Apple: subtract the desktop's own GPU load measured just before the game
                    s["gpu_ms_net"] = max(0.0, util - idle) / 100 * wall / s["frames"]
                if g.get("clock_gr"):
                    # NVIDIA: busy time at the clock the driver chose; scale to the 3080's 2100 MHz maximum
                    s["gpu_ms_maxclk"] = s["gpu_ms_est"] * g["clock_gr"] / 2100
        if k < len(recs):
            cap = [rows[i] for i in recs[k][5:]]
            zones = ["tick", "finish_step", "fence_poll", "recycle", "ui", "encode", "submit", "present", "limiter"]
            tot = [(int(r["end_ns"]) - int(r["begin_ns"])) / 1e6 for r in cap]
            shot = [t - sum(ms(r, z) for z in zones) for t, r in zip(tot, cap)]
            s["cap_n"] = len(cap); s["cap_med"] = pct(tot, 50); s["cap_p95"] = pct(tot, 95)
            s["cap_render_med"] = pct(shot, 50); s["cap_render_p95"] = pct(shot, 95)
        res["spots"][spot] = s
    return res

def main():
    root = Path(sys.argv[1]); spots = sys.argv[2:] or ["market", "view"]
    out = []
    for d in sorted(p for p in root.iterdir() if p.is_dir()):
        out.append(analyze(d, spots))
    (root / "summary.json").write_text(json.dumps(out, indent=1))
    for r in out:
        print(f"== {r['label']} [{r['flags']}] idle={r.get('idle_util')} exit={r['exit']} done={r.get('done')} errors={r['n_errors']} attempts={r.get('attempts')} overflow={r.get('overflow')}")
        for sp, s in r.get("spots", {}).items():
            g = s.get("gpu", {})
            util = g.get("util_gpu", g.get("device_util"))
            mem = g.get("mem_used_mib", g.get("in_use_mem"))
            fmt = lambda v, f="{:.1f}": "-" if v is None else f.format(v)
            print(f"   {sp:8s} fps={fmt(s['fps'])} iv={fmt(s['iv_med'],'{:.2f}')}/{fmt(s['iv_p95'],'{:.2f}')}/{fmt(s['iv_p99'],'{:.2f}')} "
                  f"enc={fmt(s['encode_med'],'{:.2f}')} pres={fmt(s['present_med'],'{:.2f}')} sim={fmt(s['sim_med'],'{:.2f}')} fb={s['fence_busy']}/{s['ticks']} "
                  f"gpu%={fmt(util)} gpu_ms~{fmt(s.get('gpu_ms_est'),'{:.2f}')} @max~{fmt(s.get('gpu_ms_maxclk'),'{:.2f}')} net~{fmt(s.get('gpu_ms_net'),'{:.2f}')} mem={fmt(mem,'{:.0f}')} clk={fmt(g.get('clock_gr'),'{:.0f}')} W={fmt(g.get('power_w'),'{:.0f}')} "
                  f"cap={fmt(s.get('cap_med'),'{:.2f}')}/{fmt(s.get('cap_p95'),'{:.2f}')} cap_render={fmt(s.get('cap_render_med'),'{:.2f}')} n={s.get('cap_n')}")
        for e in r["errors"][:8]:
            print("     !", e[:200])

if __name__ == "__main__":
    main()
