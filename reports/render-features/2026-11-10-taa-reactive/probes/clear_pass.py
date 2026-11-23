#!/usr/bin/env python3
"""The GPU time of the empty pass that only clears the TAA reactive mask (mac-cost.md).

    clear_pass.py <runs-dir> <mode> [<mode>...]

On Metal an encoder without draws has no label in `.gpu.csv` ("unlabelled"): the clear pass is the unlabelled render
encoder with 0 draws right before the encoder labelled "Translucent" of the same frame. Rows of valid runs inside the
perf segments, all views (the pass does not depend on the view): mean and median of its vertex, fragment and span.
"""
import csv
import json
import statistics
import sys
from pathlib import Path

runs, modes = Path(sys.argv[1]), sys.argv[2:]
for place in ("harbour-night", "market"):
    vs, fs, span = [], [], []
    for d in sorted(runs.iterdir()):
        st = d / "status.json"
        if not st.exists() or d.name.split(".", 1)[0] not in modes or not json.loads(st.read_text())["valid"]:
            continue
        rows = list(csv.DictReader((d / place / "repeat-1" / "frameprofile.csv.gpu.csv").open()))
        for a, b in zip(rows, rows[1:]):
            if (b["labels"] == "Translucent" and a["labels"] == "unlabelled" and a["draw_calls"] == "0"
                    and a["attempt"] == b["attempt"] and a["valid"] == "1" and a["segment"].startswith("perf-")):
                vs.append(int(a["vertex_ns"]) / 1e6); fs.append(int(a["fragment_ns"]) / 1e6)
                span.append(int(a["gpu_ns"]) / 1e6)
    if fs:
        print(f"{place}: {len(fs)} frames; fragments mean {statistics.fmean(fs):.3f} median {statistics.median(fs):.3f} ms; "
              f"vertices median {statistics.median(vs):.3f} ms; span median {statistics.median(span):.3f} ms")
