#!/usr/bin/env python3
"""Series of perf.sh runs on the Mac -> per-mode means with spread (mac-cost.md).

    analyze.py <runs-dir> [--ref MODE] [--labels Translucent,TAA] [--foreign 5] [--views]

A run is a folder `<mode>.<n>-a<attempt>` made by series3.sh; only runs with `status.json` valid=1 count (exit 0, no
other game beside it). Each view (perf-<place>/view-1..4, 8 s) also needs a quiet GPU while it ran:
`gpumon.jsonl` holds the accumulated GPU time of every process each second (ioreg, AGX user clients); a view counts
when the processes other than Gothic2Notr, WindowServer and the Claude app took less than `--foreign` % of the GPU during it.

Per view: the mean over frames of the frame's GPU time (`gpu_ns` of submitted frames) and, per label, of the
`.gpu.csv` stage times: `span` (vertex start .. fragment end, overlaps its neighbours), `vs`, `fs`.
Per mode and place: the mean over views of the per-view means over runs. A difference to the reference mode is
the mean over views of the per-view differences; ± is its standard error from the run-to-run spread.
"""
import csv
import json
import statistics
import sys
from pathlib import Path

PLACES = ("harbour-night", "market")
OWN = ("Gothic2Notr",)
# always on while the Mac is in use: the compositor and the desktop app of the agent sessions; reported, not limited
BACKGROUND = ("WindowServer", "Claude")
# older gpumon records (before v2): the limit for all GPU use beside the game, background included
LEGACY_LIMIT = 15.0


def read_gpumon(path):
    if not path.exists():
        return None
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def shares(mon, left, right):
    """GPU share (%) of foreign processes and of the background ones between two monotonic ns.

    Records of gpumon v2 (`"v": 2`) name each process right. The earlier ones took each client's usage for the client
    printed before it (ioreg sorts "AppUsage" before "IOUserClientCreator"): the names are shifted, the sum is right.
    For them the largest process is taken for the game and the rest is returned as foreign, background None."""
    inside = [s for s in mon if left <= s["t"] <= right]
    before = [s for s in mon if s["t"] < left]
    after = [s for s in mon if s["t"] > right]
    a = before[-1] if before else (inside[0] if inside else None)
    b = after[0] if after else (inside[-1] if inside else None)
    if a is None or b is None or b["t"] <= a["t"]:
        return None, None
    dt = b["t"] - a["t"]
    deltas = {key: value - a["gpu"].get(key, 0) for key, value in b["gpu"].items()}  # a new process: all inside
    deltas = {key: d for key, d in deltas.items() if d > 0}
    if b.get("v") != 2:
        rest = sum(deltas.values()) - max(deltas.values(), default=0)
        return 100.0 * rest / dt, None
    foreign = compositor = 0
    for key, d in deltas.items():
        name = key.split(" ", 1)[1]
        if name in OWN:
            continue
        if name in BACKGROUND:
            compositor += d
        else:
            foreign += d
    return 100.0 * foreign / dt, 100.0 * compositor / dt


def views_of(leaf, place, labels, mon):
    rows = list(csv.DictReader((leaf / "frameprofile.csv").open()))
    passes = {}
    for r in csv.DictReader((leaf / "frameprofile.csv.gpu.csv").open()):
        if r["valid"] != "1" or r["labels"] not in labels:
            continue
        acc = passes.setdefault(r["attempt"], {})
        for part, col in (("span", "gpu_ns"), ("vs", "vertex_ns"), ("fs", "fragment_ns")):
            key = f"{r['labels']}.{part}"
            acc[key] = acc.get(key, 0) + int(col and r[col]) / 1e6
    out = {}
    for view in range(1, 5):
        seg = f"perf-{place}/view-{view}"
        sel = [r for r in rows if r["segment"] == seg]
        frames = [r for r in sel if r["outcome"] == "1" and r.get("gpu_done") == "1" and int(r["gpu_ns"]) > 0]
        if not frames:
            continue
        left, right = int(sel[0]["begin_ns"]), int(sel[-1]["end_ns"])
        foreign, compositor = shares(mon, left, right) if mon else (None, None)
        v = {"frame": statistics.fmean(int(r["gpu_ns"]) / 1e6 for r in frames),
             "frame_med": statistics.median(int(r["gpu_ns"]) / 1e6 for r in frames),
             "foreign": foreign, "compositor": compositor}
        for label in labels:
            for part in ("span", "vs", "fs"):
                key = f"{label}.{part}"
                xs = [passes[r["attempt"]][key] for r in sel if key in passes.get(r["attempt"], {})]
                if xs:
                    v[key] = statistics.fmean(xs)
        out[view] = v
    return out


def main():
    runs = Path(sys.argv[1])
    ref, labels, limit, show_views = None, ["Translucent", "TAA"], 5.0, False
    args = sys.argv[2:]
    while args:
        if args[0] == "--ref":
            ref, args = args[1], args[2:]
        elif args[0] == "--labels":
            labels, args = args[1].split(","), args[2:]
        elif args[0] == "--foreign":
            limit, args = float(args[1]), args[2:]
        elif args[0] == "--views":
            show_views, args = True, args[1:]
        else:
            raise SystemExit(f"unknown {args[0]}")
    data = {}   # mode -> place -> view -> [values of runs]
    order, dropped, counted = [], [], []
    for d in sorted(runs.iterdir(), key=lambda p: p.stat().st_mtime):
        st = d / "status.json"
        if not st.exists():
            continue
        s = json.loads(st.read_text())
        if not s["valid"]:
            dropped.append(f"{d.name}: {s['why'].strip()} (before {s['before']} %, after {s['after']} %)")
            continue
        mode = s["tag"].rsplit(".", 1)[0]
        if mode not in order:
            order.append(mode)
        mon = read_gpumon(d / "gpumon.jsonl")
        for place in PLACES:
            for view, v in views_of(d / place / "repeat-1", place, labels, mon).items():
                ok = v["foreign"] is not None and v["foreign"] < (limit if v["compositor"] is not None else LEGACY_LIMIT)
                line = (f"{d.name} {place} view-{view}: foreign {v['foreign']:.1f} %, background "
                        f"{v['compositor'] if v['compositor'] is not None else float('nan'):.1f} %, frame {v['frame']:.2f} ms" if v["foreign"] is not None
                        else f"{d.name} {place} view-{view}: no gpumon")
                (counted if ok else dropped).append(line)
                if ok:
                    data.setdefault(mode, {}).setdefault(place, {}).setdefault(view, []).append(v)
    cols = ["frame", "frame_med"] + [f"{lab}.{part}" for lab in labels for part in ("span", "vs", "fs")]

    def mode_place(mode, place, col):
        per_view = data.get(mode, {}).get(place, {})
        means = [statistics.fmean(x[col] for x in vs if col in x) for vs in per_view.values() if vs]
        ns = [len(vs) for vs in per_view.values()]
        return (statistics.fmean(means) if len(means) == 4 else None), ns

    for place in PLACES:
        print(f"\n## {place}: mean over the 4 views of the per-view mean over clean runs, ms (runs per view)")
        print("| mode | runs | " + " | ".join(cols) + " |")
        print("|---" * (len(cols) + 2) + "|")
        for mode in order:
            cells, ns = [], []
            for c in cols:
                m, ns = mode_place(mode, place, c)
                cells.append("—" if m is None else f"{m:.3f}")
            print(f"| {mode} | {'/'.join(map(str, ns))} | " + " | ".join(cells) + " |")
        if ref and ref in data:
            print(f"\n### {place}: difference to {ref}, ms (± standard error from the spread of runs)")
            print("| mode | " + " | ".join(cols) + " |")
            print("|---" * (len(cols) + 1) + "|")
            for mode in order:
                if mode == ref:
                    continue
                cells = []
                for c in cols:
                    diffs, var = [], 0.0
                    for view in range(1, 5):
                        a = [x[c] for x in data.get(mode, {}).get(place, {}).get(view, []) if c in x]
                        b = [x[c] for x in data.get(ref, {}).get(place, {}).get(view, []) if c in x]
                        if not a or not b:
                            break
                        diffs.append(statistics.fmean(a) - statistics.fmean(b))
                        var += (statistics.variance(a) / len(a) if len(a) > 1 else float("nan")) + \
                               (statistics.variance(b) / len(b) if len(b) > 1 else float("nan"))
                    if len(diffs) != 4:
                        cells.append("—"); continue
                    se = var ** 0.5 / 4
                    cells.append(f"{statistics.fmean(diffs):+.3f} ± {se:.3f}")
                print(f"| {mode} | " + " | ".join(cells) + " |")
    print(f"\n## dropped ({len(dropped)})")
    for line in dropped:
        print("-", line)
    if show_views:
        print(f"\n## counted views ({len(counted)})")
        for line in counted:
            print("-", line)


if __name__ == "__main__":
    main()
