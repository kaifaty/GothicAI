"""GPU-resident near/far eye replay using Warp mesh queries."""
import gc
import json
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
import warp as wp

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from formats import read_rays, read_static


@wp.func
def cast_segment(mesh: wp.uint64, origin: wp.vec3, destination: wp.vec3, cull: int):
    maximum = wp.length(destination - origin)
    direction = wp.normalize(destination - origin)
    offset = float(0.0)
    # Warp has no traversal filter. Reject backfaces and advance beyond their
    # plane by a scale-aware float32 epsilon; agreement quantifies this bias.
    epsilon = wp.max(0.001, 4.0e-7 * wp.max(wp.abs(origin[0]), wp.max(wp.abs(origin[1]), wp.abs(origin[2]))))
    while offset < maximum:
        query = wp.mesh_query_ray(mesh, origin + direction * offset, direction, maximum - offset)
        if not query.result:
            return wp.vec2(maximum, 0.0)
        if cull == 0 or wp.dot(direction, query.normal) < 0.0:
            return wp.vec2(offset + query.t, 1.0)
        offset = offset + query.t + epsilon
    return wp.vec2(maximum, 0.0)


@wp.kernel
def replay(mesh: wp.uint64, origins: wp.array(dtype=wp.vec3), dirs: wp.array(dtype=wp.vec3),
           ranges: wp.array(dtype=float), far: wp.array(dtype=int), near_range: float, cull: int,
           distance: wp.array(dtype=float), hits: wp.array(dtype=int)):
    i = wp.tid()
    o = origins[i // 238]
    direction = dirs[i]
    maximum = ranges[i]
    end = wp.min(near_range, maximum)
    query = cast_segment(mesh, o, o + direction * end, cull)
    value = end
    hit = int(0)
    if query[1] != 0.0:
        value = query[0]
        hit = 1
    elif far[i // 238] != 0 and end < maximum:
        near_point = o + direction * end
        far_point = o + direction * maximum
        query_far = cast_segment(mesh, near_point, far_point, cull)
        value = maximum
        if query_far[1] != 0.0:
            value = wp.length(near_point + wp.normalize(far_point - near_point) * query_far[0] - o)
            hit = 1
    distance[i] = value
    hits[i] = hit


def main(inputs, raw):
    wp.init()
    if not wp.is_cuda_available():
        raise RuntimeError("Warp has no available CUDA device")
    wp.set_device("cuda:0")
    device = wp.get_device()
    wp.config.kernel_cache_dir = str(raw / "warp-cache")
    static = read_static(inputs / "static.bin")
    rays = read_rays(inputs / "rays.bin")
    vertices = wp.array(np.asarray(static.vertices_cm).reshape(-1, 3), dtype=wp.vec3)
    indices = wp.array(np.arange(static.vertices_cm.size // 3, dtype=np.int32), dtype=int)
    build_times = []
    for repeat in range(3):
        wp.synchronize()
        start = time.perf_counter()
        mesh = wp.Mesh(points=vertices, indices=indices, support_winding_number=False)
        wp.synchronize()
        build_times.append((time.perf_counter() - start) * 1000)
        if repeat < 2:
            del mesh
            gc.collect()
    reference_dtype = np.dtype([("distance", "<f4"), ("normal", "<f4"), ("kind", "<u4"), ("flags", "<u4")])
    references = {cull: np.memmap(raw / ("embree1-cull.hits" if cull else "embree1-two-sided.hits"), dtype=reference_dtype, mode="r", shape=(len(rays.updates), 238)) for cull in (0, 1)}
    result = {"status": "measured", "warp": wp.__version__, "device": str(device), "device_name": device.name,
              "bvh_build_ms": {"median": float(np.median(build_times)), "p90": float(np.percentile(build_times, 90)), "repeats": 3},
              "bvh_build_raw_ms": build_times, "bvh_warmup": "CUDA initialization complete; three cold mesh builds; transfers excluded",
              "backfaces": "Native two-sided plus iterative front-face variant: reject dot(dir, normal)>=0 and recast beyond plane by max(0.001cm,4e-7*max(abs(origin))) float32 epsilon. May skip closer/coplanar faces inside epsilon; agreement measured against matching Embree variant.",
              "timing": "CUDA events, GPU-resident buffers; transfers/JIT excluded; 5 warmup launches then 30 timed launches",
              "batches": []}
    for agents, cull in ((n, c) for n in (64, 1024, 4096) for c in (0, 1)):
        # Equally spaced independent eyes across the entire 24000-update corpus.
        chosen = np.linspace(0, len(rays.updates)-1, agents, dtype=int)
        batch = rays.updates[chosen]
        origins = wp.array(np.ascontiguousarray(batch["origin"]), dtype=wp.vec3)
        dirs = wp.array(np.ascontiguousarray(batch["rays"]["direction"]).reshape(-1, 3), dtype=wp.vec3)
        ranges = wp.array(np.ascontiguousarray(batch["rays"]["max_range_cm"]).reshape(-1), dtype=float)
        far = wp.array(batch["far_updated"].astype(np.int32), dtype=int)
        distances = wp.empty(agents*238, dtype=float)
        hits = wp.empty(agents*238, dtype=int)
        args = [mesh.id, origins, dirs, ranges, far, rays.near_range_cm, cull, distances, hits]
        for _ in range(5):
            wp.launch(replay, agents*238, inputs=args)
        wp.synchronize()
        timings = []
        for _ in range(30):
            begin, end = wp.Event(enable_timing=True), wp.Event(enable_timing=True)
            wp.record_event(begin)
            wp.launch(replay, agents*238, inputs=args)
            wp.record_event(end)
            wp.synchronize_event(end)
            timings.append(wp.get_event_elapsed_time(begin, end))
        actual = distances.numpy().reshape(agents, 238)
        actual_hit = hits.numpy().reshape(agents, 238) != 0
        expected = references[cull][chosen]
        expected_hit = (expected["flags"] & 1) != 0
        delta = np.abs(actual - expected["distance"])
        # The per-process figure includes CUDA context and allocator overhead;
        # gq additionally reserves 3 GiB before admitting this phase.
        usage = subprocess.check_output(["nvidia-smi", "--query-compute-apps=pid,used_gpu_memory", "--format=csv,noheader,nounits"], text=True)
        import os
        memory_mib = next((int(line.split(",")[1]) for line in usage.splitlines() if int(line.split(",")[0]) == os.getpid()), None)
        if memory_mib is None:
            raise RuntimeError("Cannot verify process GPU memory through nvidia-smi")
        if memory_mib > 3072:
            raise RuntimeError(f"Warp used {memory_mib} MiB, above the 3 GiB reservation")
        both_hit = actual_hit & expected_hit
        result["batches"].append({"agents": agents, "cull": bool(cull), "eye_rays": agents*238, "repeats": 30, "warmup": 5,
                                  "batch_ms": {"median": float(np.median(timings)), "p90": float(np.percentile(timings, 90))},
                                  "eye_rays_per_second": {"median": float(np.median(agents*238*1000/np.array(timings))), "p90": float(np.percentile(agents*238*1000/np.array(timings), 90))},
                                  "process_gpu_memory_mib": memory_mib, "hit_equal_fraction": float((actual_hit == expected_hit).mean()),
                                  "distance_lt_1cm_fraction": float((delta < 1).mean()),
                                  "both_hit_distance_delta_cm": {"median": float(np.median(delta[both_hit])), "p90": float(np.percentile(delta[both_hit], 90)), "max": float(delta[both_hit].max())}})
        np.savez(raw / f"gpu-{agents}-cull{cull}.npz", update_indices=chosen, distance_cm=actual, hit=actual_hit, timing_ms=timings)
        del origins, dirs, ranges, far, distances, hits, args
        gc.collect()
    (raw / "gpu.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
