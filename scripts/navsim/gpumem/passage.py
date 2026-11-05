#!/usr/bin/env python3
"""Compare GPU passage verdicts with unmodified NavigationPassage::check.

Replay each memory-cost.bin update in order. At every readout compare random
segments and boundary cases against that exact view, including live hazard
cells selected from both complete engine layers. With the 24 drive captures,
192 segments per view give 1,085,376 comparisons.
"""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench  # noqa: E402
import captures  # noqa: E402
import gpumem  # noqa: E402

CATEGORIES = ("random", "zero_length", "near_exit", "far_only", "cell_edge", "wall", "ledge", "drop", "water")


def segments(pose, flags, rng, count):
    """Finite world segments, with real hazard targets and exact local edges."""
    local = rng.uniform(-11000, 11000, (count, 2, 2)).astype(np.float32)
    labels = np.full(count, "random", dtype=object)
    # Half the random cases stay close enough to traverse several near cells.
    local[:count // 2] = rng.uniform(-1000, 1000, (count // 2, 2, 2))
    corridor = rng.choice(np.array([0, 12.5, 15, 20, 25, 35], np.float32), count)
    footprint = rng.choice(np.array([0, 15, 25, 30, 60], np.float32), count)
    for i in range(0, count, 12):
        local[i, 1] = local[i, 0]
        labels[i] = "zero_length"
    for i in range(1, count, 12):
        local[i] = ((0, 0), (0, 800 - corridor[i] + rng.choice([-0.001, 0, 0.001, 200])))
        labels[i] = "near_exit"
    for i in range(2, count, 12):
        local[i] = ((-2000, -9000), (2000, -1000))  # far behind the policy's crop
        labels[i] = "far_only"
    for i in range(3, count, 12):
        edge = np.float32(rng.integers(-30, 30) * 25)
        edge = np.nextafter(edge, np.float32(rng.choice([-np.inf, np.inf]))) if i % 2 else edge
        local[i] = ((edge, -700), (edge, 700))
        corridor[i] = 0
        labels[i] = "cell_edge"
    targeted = Counter()
    for kind, bit in (("wall", 1), ("ledge", 2), ("drop", 4), ("water", 8)):
        choices = np.flatnonzero(flags & bit)
        if not len(choices):
            continue
        for i in range(4 + bit.bit_length() - 1, count, 12):
            index = int(rng.choice(choices))
            size, pitch, offset = (64, 25, 0) if index < 4096 else (100, 200, 4096)
            z, x = divmod(index - offset, size)
            center = np.array([(x + .5 - size * .5) * pitch, (z + .5 - size * .5) * pitch], np.float32)
            local[i] = (center - (pitch, 0), center + (pitch, 0))
            corridor[i], footprint[i], labels[i] = 15, 0, kind
            targeted[kind] += 1
    # Frame() renormalises the stored forward, so generate in that frame too.
    forward = pose[[3, 5]].copy()
    norm = np.float32(np.sqrt(np.float32((forward * forward).sum())))
    forward *= np.float32(1) / norm
    right = np.array([-forward[1], forward[0]], np.float32)
    q = np.zeros((count, 8), np.float32)
    for endpoint in range(2):
        world = pose[[0, 2]] + local[:, endpoint, :1] * right + local[:, endpoint, 1:] * forward
        q[:, endpoint * 3], q[:, endpoint * 3 + 2] = world[:, 0], world[:, 1]
        q[:, endpoint * 3 + 1] = pose[1]
    q[:, 6], q[:, 7] = corridor, footprint
    return q, labels, targeted


def run_parity(backend, records, reference, weights, *, per_view=192):
    refs = [bench.Reference(reference, c.path, weights) for c in records]
    maps, ids = [], []
    for c in records:
        ids.append(len(maps) if len(c.tiles) else -1)
        if len(c.tiles):
            maps.append((c.tiles, c.mask))
    memory = gpumem.Memory(backend, len(records), records[0].slots, bench.HISTORY,
                           [r.journey for r in refs], maps, ids)
    table = gpumem.EyeTable(backend, records)
    rng = np.random.default_rng(20261105)
    category = {name: Counter() for name in CATEGORIES}
    totals, targeted, views = Counter(), Counter(), np.zeros(len(records), np.int64)
    first = None
    started = time.perf_counter()
    for u in range(max(c.updates for c in records)):
        active = np.array([u < c.updates for c in records])
        updates = np.where(active, table.first[:-1] + u, -1).astype(np.int32)
        memory.scatter(table, bench.device_ints(backend, updates))
        memory.update()
        query, agents, expected, names, origins = [], [], [], [], []
        for a, (c, ref) in enumerate(zip(records, refs)):
            if not active[a]:
                continue
            for _ in range(int(c.views_after[u])):
                if not ref.next() or ref.updates.value != u + 1:
                    raise RuntimeError("passage reference and replay readout differ")
                pose, flags = ref.passage_view()
                q, labels, hits = segments(pose, flags, rng, per_view)
                query.append(q)
                agents.extend([a] * per_view)
                expected.append(ref.passage(q))
                names.extend(labels)
                origins.extend([(a, int(views[a]))] * per_view)
                targeted.update(hits)
                views[a] += 1
        if not query:
            continue
        q = np.concatenate(query)
        wanted = np.concatenate(expected)
        gpu_agents = bench.device_ints(backend, agents)
        if backend.kind == "cuda":
            gpu_segments = backend.torch.from_numpy(q).to("cuda")
            gpu_verdicts = backend.torch.empty(len(q), dtype=backend.torch.uint8, device="cuda")
        else:
            gpu_segments, gpu_verdicts = q, np.empty(len(q), np.uint8)
        memory.passage(gpu_agents, gpu_segments, gpu_verdicts)
        actual = bench.host(backend, gpu_verdicts)
        same = actual == wanted
        totals.update(compared=len(q), equal=int(same.sum()))
        for name in CATEGORIES:
            selected = np.asarray(names) == name
            category[name].update(compared=int(selected.sum()), equal=int(same[selected].sum()),
                                  unknown=int((wanted[selected] == 0).sum()),
                                  free=int((wanted[selected] == 1).sum()), closed=int((wanted[selected] == 2).sum()))
        if not same.all() and first is None:
            k = int(np.flatnonzero(~same)[0])
            a, view = origins[k]
            first = dict(capture=records[a].path, update=u, view=view, category=names[k],
                         segment=q[k].tolist(), engine=int(wanted[k]), actual=int(actual[k]))
    for ref in refs:
        if ref.next():
            raise RuntimeError("passage reference has extra readouts")
        ref.close()
    errors = memory.get("errors")
    return dict(backend=backend.kind, captures=len(records), views=int(views.sum()),
                segments_per_view=per_view, **totals, equal_fraction=totals["equal"] / max(1, totals["compared"]),
                first_mismatch=first, categories=category, targeted_live_hazard_segments=targeted,
                state_errors=int(np.bitwise_or.reduce(errors)), seconds=time.perf_counter() - started)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("host", "cuda"), default="cuda")
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--captures", nargs="+", required=True)
    parser.add_argument("--segments-per-view", type=int, default=192)
    parser.add_argument("--min-segments", type=int, default=1000000)
    args = parser.parse_args()
    result = run_parity(gpumem.Backend(args.backend, args.build), [captures.read(p) for p in args.captures],
                        args.reference, args.weights, per_view=args.segments_per_view)
    bench.write_json(args.output, result)
    print(f"passage: {result['equal']}/{result['compared']} equal, {result['state_errors']} state errors", flush=True)
    if result["equal"] != result["compared"] or result["state_errors"] or result["compared"] < args.min_segments:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
