#!/usr/bin/env python3
"""GPU cost of perf.sh runs for the hard VSM lamp shadows on the Mac (OPENGOTHIC_GPU_PASSES=1).

    python3 vsm_passes.py <variant>=<perf dir>[:<place>] [...]

':<place>' takes only that place of the run (a run whose other places met another game on the shared Mac).

Per run and place: the frame mean and GPU mean of report.json, and the mean per frame of encoder groups by their
labels (rows that share labels in one frame are summed; rows over 1 s are broken samples and are left out):
- light: the encoder with 'Point lights' — on Metal one render encoder for the sun (cascades or VSM), the ambient
  light, the lamps and the sky (reports/render-features/2026-11-04-lamp-pcss/lamp_passes.py);
- pages: the VSM page passes ('VSM-pages', 'VSM-pages-alloc', 'VSM-visibility' share a compute encoder with the
  epipolar fog; 'VSM-rendering' draws the pages);
- casc: the sun's two cascade maps of -vsm 0 ('ShadowMap #0', 'ShadowMap #1': depth only, so their rows are the
  vertex and depth work);
- fog: the compute encoder with 'Fog-LUTs' — irradiance, exposure, SSAO, the fog LUTs and the fog's sun occlusion
  (VSM's epipolar trace 'VSM-epipolar-fog' with fog_epipolar_occlusion.comp, or fog3d.comp over the cascade).
Each group is printed as the row (gpu_ns) and its fragment stage. A render encoder starts its vertices early and
waits, so a row can be longer than its work; the lighting row is what step 2c compared.
Then the mean over the runs of each variant.
"""
import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

GROUPS = ('light', 'pages', 'casc', 'fog')


def group(labels: str) -> str | None:
    if 'Point lights' in labels:
        return 'light'
    if 'VSM-pages' in labels or 'VSM-rendering' in labels:
        return 'pages'
    if 'ShadowMap' in labels:
        return 'casc'
    if 'Fog-LUTs' in labels:
        return 'fog'
    return None


def encoders(gpu_csv: Path) -> dict[str, tuple[float, float]]:
    frames = set()
    sums: dict[str, list[float]] = defaultdict(lambda: [0.0, 0.0])
    with gpu_csv.open() as source:
        for row in csv.DictReader(source):
            if not row['segment'].startswith('perf-'):
                continue
            frames.add((row['segment'], row['frame']))
            if row['valid'] != '1' or int(row['gpu_ns']) >= 1e9:
                continue
            g = group(row['labels'])
            if g is None:
                continue
            sums[g][0] += int(row['gpu_ns'])/1e6
            sums[g][1] += int(row['fragment_ns'])/1e6
    n = max(1, len(frames))
    return {g: (v[0]/n, v[1]/n) for g, v in sums.items()}


def main() -> None:
    table: dict[tuple[str, str], list[dict[str, float]]] = defaultdict(list)
    for arg in sys.argv[1:]:
        variant, _, root = arg.partition('=')
        root, _, only = root.partition(':')
        root = Path(root)
        report = json.loads((root/'report.json').read_text())
        for place, data in sorted(report['places'].items()):
            if only and place != only:
                continue
            s = data['summary']
            leaf = root/place/'repeat-1'
            meta = json.loads((leaf/'metadata.json').read_text())
            enc = encoders(leaf/'frameprofile.csv.gpu.csv')
            cells = {'frame': s['frame_ms']['mean'], 'gpu': s['gpu']['mean_ms']}
            for g in GROUPS:
                row, frag = enc.get(g, (0.0, 0.0))
                cells[g], cells[g + '_frag'] = row, frag
            table[(place, variant)].append(cells)
            print(f'{variant:6s} {root.name} {place:13s} {meta["drawable_width"]}x{meta["drawable_height"]} '
                  f'frame {cells["frame"]:6.2f}  gpu {cells["gpu"]:6.2f}  ' +
                  '  '.join(f'{g} {cells[g]:6.2f}/{cells[g + "_frag"]:5.2f}' for g in GROUPS))
    print()
    print('mean over runs: place variant runs frame gpu light(row/frag) pages(row/frag) casc(row/frag) fog(row/frag)')
    for (place, variant), runs in sorted(table.items()):
        m = {k: statistics.mean(r[k] for r in runs) for k in runs[0]}
        gpus = ' / '.join(f'{r["gpu"]:.2f}' for r in runs)
        print(f'{place:13s} {variant:6s} {len(runs)}  frame {m["frame"]:6.2f}  gpu {m["gpu"]:6.2f} ({gpus})  ' +
              '  '.join(f'{g} {m[g]:6.2f}/{m[g + "_frag"]:5.2f}' for g in GROUPS))


if __name__ == '__main__':
    main()
