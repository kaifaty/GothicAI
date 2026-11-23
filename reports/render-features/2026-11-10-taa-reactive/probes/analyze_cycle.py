#!/usr/bin/env python3
"""Runs of the phase-cycling probe build (cycle.patch, OPENGOTHIC_PROBE_CYCLE=<ms>) -> paired differences per phase.

    analyze_cycle.py <runs-dir> [--mode C] [--phase-ms 250] [--skip-ms 60] [--foreign 5] [--labels Translucent,TAA] [--table c|c2|fix]

--mode is the series tag of the runs (the part of `<tag>.<n>-a<attempt>` before the dot, start-s4.sh / start-fix.sh), and
--table the phase table of the build those runs ran:
    series s4:  --mode C --table c    (build of cycle-c.patch)
                --mode D --table c2   (build of cycle-c2.patch: the tag D is that build, `C2` in the binaries' folder)
    series fix: --mode F --table fix  (build of cycle-fix.patch, on top of the fix)
A pair element may be a tuple of phases that run the same mode: their per-view means are averaged.

Each frame's phase is (begin_ns / phase) % 8 of its frameprofile.csv row (steady_clock, as Renderer::updateTaaProbe).
A frame counts when its row begins at least --skip-ms after its phase started (the GPU of the last phase's frames is
done by then) and ends inside the same phase. A view counts when the foreign processes took < --foreign % of the GPU
during it (gpumon.jsonl, analyze.py). Per view and phase: the mean over frames. A difference between two phases is
taken inside each view (the same camera and the same background load), then averaged over the views of all runs of a
place; ± is the standard error over those views.
"""
import csv
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from analyze import LEGACY_LIMIT, PLACES, read_gpumon, shares  # noqa: E402

# the phase tables of Renderer::updateTaaProbe: the first cycle build (cycle-c.patch) and the second (cycle-c2.patch)
TABLES = {
    "c": (["O (mask off)", "O + no attachment", "O + attachment not stored", "O + attachment not cleared",
           "O + resolve without mask", "O + no pfx push data", "Z (all of the mask out)", "N (main, mask on)"],
          [(1, 0), (2, 0), (3, 0), (4, 0), (5, 0), (6, 0), (7, 0), (7, 6)]),
    "c2": (["N (main, mask on)", "N + mask cleared by its own pass", "O (mask off)", "O + no attachment",
            "O + resolve without mask", "Z (all of the mask out)", "N again (null pair)", "O + mask cleared by its own pass"],
           [(6, 0), (0, 5), (1, 0), (1, 5), (2, 5), (3, 2), (4, 2), (5, 2), (0, 2), (7, 2)]),
    "fix": (["F (the fix)", "N (main's clear in the translucent pass)", "mask off, no attachment",
             "Z (all of the mask out)", "F again", "N again", "mask off, no attachment, again", "Z again"],
            [(4, 0), (5, 1), ((0, 4), (1, 5)), ((0, 4), (2, 6)), ((0, 4), (3, 7)), ((1, 5), (2, 6)), ((1, 5), (3, 7))]),
}


def value(means, phase, col):
    """The per-view mean of a phase, or of several phases of the same mode."""
    phases = phase if isinstance(phase, tuple) else (phase,)
    xs = [means[ph][col] for ph in phases if col in means.get(ph, {})]
    return statistics.fmean(xs) if xs else None


def label(phase):
    return "+".join(map(str, phase)) if isinstance(phase, tuple) else str(phase)


def view_phases(leaf, place, labels, phase_ns, skip_ns):
    rows = list(csv.DictReader((leaf / "frameprofile.csv").open()))
    passes = {}
    for r in csv.DictReader((leaf / "frameprofile.csv.gpu.csv").open()):
        if r["valid"] != "1" or r["labels"] not in labels:
            continue
        acc = passes.setdefault(r["attempt"], {})
        for part, col in (("span", "gpu_ns"), ("vs", "vertex_ns"), ("fs", "fragment_ns")):
            key = f"{r['labels']}.{part}"
            acc[key] = acc.get(key, 0) + int(r[col]) / 1e6
    out = {}
    for view in range(1, 5):
        seg = f"perf-{place}/view-{view}"
        sel = [r for r in rows if r["segment"] == seg]
        if not sel:
            continue
        window = (int(sel[0]["begin_ns"]), int(sel[-1]["end_ns"]))
        per = {}
        for r in sel:
            if r["outcome"] != "1" or r.get("gpu_done") != "1" or int(r["gpu_ns"]) <= 0:
                continue
            b, e = int(r["begin_ns"]), int(r["end_ns"])
            if b // phase_ns != e // phase_ns or b % phase_ns < skip_ns:
                continue
            ph = (b // phase_ns) % 8
            v = {"frame": int(r["gpu_ns"]) / 1e6, **passes.get(r["attempt"], {})}
            per.setdefault(ph, []).append(v)
        out[view] = (window, per)
    return out


def main():
    runs = Path(sys.argv[1])
    mode, phase_ms, skip_ms, limit, labels, table = "C", 250, 60, 5.0, ["Translucent", "TAA"], "c"
    args = sys.argv[2:]
    while args:
        k, v, args = args[0], args[1], args[2:]
        if k == "--mode": mode = v
        elif k == "--phase-ms": phase_ms = int(v)
        elif k == "--skip-ms": skip_ms = int(v)
        elif k == "--foreign": limit = float(v)
        elif k == "--labels": labels = v.split(",")
        elif k == "--table": table = v
        else: raise SystemExit(f"unknown {k}")
    PHASES, PAIRS = TABLES[table]
    cols = ["frame"] + [f"{lab}.{part}" for lab in labels for part in ("fs", "vs", "span")]
    units = {p: [] for p in PLACES}  # per place: list of (run, view, {phase: {col: mean}}, counts)
    dropped = []
    for d in sorted(runs.iterdir(), key=lambda p: p.stat().st_mtime):
        st = d / "status.json"
        if not st.exists():
            continue
        s = json.loads(st.read_text())
        if s["tag"].rsplit(".", 1)[0] != mode:
            continue
        if not s["valid"]:
            dropped.append(f"{d.name}: {s['why'].strip()} (before {s['before']} %, after {s['after']} %)")
            continue
        mon = read_gpumon(d / "gpumon.jsonl")
        for place in PLACES:
            for view, (window, per) in view_phases(d / place / "repeat-1", place, labels,
                                                   phase_ms * 1000000, skip_ms * 1000000).items():
                foreign, background = shares(mon, *window) if mon else (None, None)
                # older gpumon records: the rest beside the game also holds WindowServer and the Claude app (7-13 %)
                if foreign is None or foreign >= (limit if background is not None else LEGACY_LIMIT):
                    dropped.append(f"{d.name} {place} view-{view}: foreign "
                                   f"{'—' if foreign is None else f'{foreign:.1f}'} %")
                    continue
                if len(per) < 8:
                    dropped.append(f"{d.name} {place} view-{view}: {len(per)} phases")
                    continue
                means = {ph: {c: statistics.fmean(x[c] for x in xs if c in x) for c in cols
                              if any(c in x for x in xs)} for ph, xs in per.items()}
                units[place].append((d.name, view, means, {ph: len(xs) for ph, xs in per.items()},
                                     foreign, background))
    for place in PLACES:
        us = units[place]
        print(f"\n## {place}: {len(us)} views (runs × 4), frames per phase per view: "
              f"{min((min(u[3].values()) for u in us), default=0)}…{max((max(u[3].values()) for u in us), default=0)}")
        if not us:
            continue
        print("\nMean per phase, ms:\n")
        print("| phase | " + " | ".join(cols) + " |")
        print("|---" * (len(cols) + 1) + "|")
        for ph, name in enumerate(PHASES):
            print(f"| {ph} {name} | " + " | ".join(
                f"{statistics.fmean(u[2][ph][c] for u in us):.3f}" for c in cols) + " |")
        print("\nDifference inside each view, mean over views ± standard error, ms:\n")
        print("| pair | " + " | ".join(cols) + " |")
        print("|---" * (len(cols) + 1) + "|")
        for a, b in PAIRS:
            cells = []
            for c in cols:
                ds = [value(u[2], a, c) - value(u[2], b, c) for u in us
                      if value(u[2], a, c) is not None and value(u[2], b, c) is not None]
                se = statistics.stdev(ds) / len(ds) ** 0.5 if len(ds) > 1 else float("nan")
                cells.append(f"{statistics.fmean(ds):+.3f} ± {se:.3f}")
            print(f"| {label(a)} − {label(b)} | " + " | ".join(cells) + " |")
        bg = [u[5] for u in us if u[5] is not None] or [float("nan")]
        fg = [u[4] for u in us]
        print(f"\nforeign GPU during the counted views: {min(fg):.1f}…{max(fg):.1f} %, "
              f"WindowServer + Claude app: {min(bg):.1f}…{max(bg):.1f} %")
    print(f"\n## dropped ({len(dropped)})")
    for line in dropped:
        print("-", line)


if __name__ == "__main__":
    main()
