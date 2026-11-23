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
    for row, i in enumerate(range(3, count, 12)):
        edge = np.float32(rng.integers(-30, 30) * 25)
        edge = np.nextafter(edge, np.float32(rng.choice([-np.inf, np.inf]))) if row % 2 else edge
        local[i] = ((edge, -700), (edge, 700))
        corridor[i] = 0
        labels[i] = "cell_edge"
    # Near hazards close even with free evidence; far hazards close only when
    # the coarse cell has no free evidence (bit 32 from the engine view).
    near_flags, far_flags = flags[:4096], flags[4096:]
    far_z, far_x = np.divmod(np.arange(10000), 100)
    far_centers = np.stack(((far_x + .5 - 50) * 200, (far_z + .5 - 50) * 200), 1)
    outside_near = np.max(np.abs(far_centers), axis=1) >= 900
    far_free = np.flatnonzero(outside_near & ((far_flags & 16) != 0)) + 4096
    far_closed = np.flatnonzero(outside_near & ((far_flags & 15) != 0) & ((far_flags & 32) == 0)) + 4096
    near_free = np.flatnonzero(near_flags & 16)
    near_closed = np.flatnonzero(near_flags & 15)

    def place(rows, choices, *, zero=False, edge=False):
        if not len(rows) or not len(choices):
            return 0
        selected = rng.choice(choices, len(rows))
        for i, index in zip(rows, selected):
            size, pitch, offset = (64, 25, 0) if index < 4096 else (100, 200, 4096)
            z, x = divmod(int(index) - offset, size)
            center = np.array([(x + .5 - size * .5) * pitch, (z + .5 - size * .5) * pitch], np.float32)
            if edge:
                # Parallel to an observed near-cell edge, with a 0.001 cm
                # inward offset; untouched rows retain exact/nextafter edges.
                center[0] -= pitch * .5 - .001
                delta = np.array((0, pitch * .1), np.float32)
            else:
                delta = np.array((0 if zero else pitch * .1, 0), np.float32)
            local[i] = (center - delta, center + delta)
            corridor[i], footprint[i] = 0, 0
        return len(rows)

    # Keep an unknown far-only case too, and exercise both verdict branches
    # using nonzero segments entirely inside real observed far cells.
    far_rows = np.arange(2, count, 12)
    place(far_rows[::3], far_free)
    place(far_rows[1::3], far_closed)
    zero_rows = np.arange(0, count, 12)
    place(zero_rows[::3], near_free, zero=True)
    place(zero_rows[1::3], near_closed, zero=True)
    edge_rows = np.arange(3, count, 12)
    place(edge_rows[::3], near_free, edge=True)
    place(edge_rows[1::3], near_closed, edge=True)

    targeted = Counter()
    for kind, bit in (("wall", 1), ("ledge", 2), ("drop", 4), ("water", 8)):
        # A drop label must name an actual closing height discontinuity, not
        # a far cell whose simultaneous free evidence makes it Unknown.
        choices = np.concatenate((np.flatnonzero(near_flags & bit),
                                  np.flatnonzero(outside_near & ((far_flags & bit) != 0)
                                                 & ((far_flags & 32) == 0)) + 4096))
        rows = np.arange(4 + bit.bit_length() - 1, count, 12)
        if place(rows, choices):
            labels[rows] = kind
            targeted[kind] += len(rows)
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
    required = {name: ("free", "closed") for name in CATEGORIES[:5]}
    required.update({name: ("closed",) for name in CATEGORIES[5:]})
    missing = [f"{name}.{verdict}" for name, verdicts in required.items()
               for verdict in verdicts if category[name][verdict] == 0]
    return dict(backend=backend.kind, captures=len(records), views=int(views.sum()),
                segments_per_view=per_view, **totals, equal_fraction=totals["equal"] / max(1, totals["compared"]),
                first_mismatch=first, categories=category, missing_coverage=missing, targeted_live_hazard_segments=targeted,
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
    for name, counts in result["categories"].items():
        print(f"  {name}: free={counts['free']} closed={counts['closed']} unknown={counts['unknown']}", flush=True)
    if result["missing_coverage"]:
        print("missing coverage: " + ", ".join(result["missing_coverage"]), flush=True)
    if (result["equal"] != result["compared"] or result["state_errors"]
            or result["compared"] < args.min_segments or result["missing_coverage"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
