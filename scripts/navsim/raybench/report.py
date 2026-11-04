"""Prepare measured NPC dimensions and condense standalone raw results."""
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


def write(path, data):
    path.write_text(json.dumps(data, indent=2, allow_nan=False) + "\n")


def prepare(inputs, raw):
    # Only the currently selected imported base is read; no import or engine build.
    fingerprint = json.loads((Path.home() / "gothic-ai/game-base.json").read_text())["base"]
    base = Path.home() / ".cache/gothic-ai/base" / fingerprint / "models"
    hierarchy = base / "HUMANS.MDH.json"
    data = json.loads(hierarchy.read_text())["data"]
    low, high = data["collision_bbox"]
    size = [b - a for a, b in zip(low, high)]
    radius = min(size[1] * .5, size[0], size[2]) * .5
    height = size[1]
    # Running speed from the root translation of S_RUNL, using the same
    # quantized samples/elapsed frames as the animation loader, in cm/s.
    animation = base / "HUMANS-S_RUNL.MAN.json"
    meta = json.loads(animation.read_text())
    anim = meta["data"]
    assert anim["samples"]["encoding"] == "quantized16"
    blob = animation.with_name(meta["buffer"]).read_bytes()
    positions = []
    for frame in (0, anim["frame_count"] - 1):
        q = struct.unpack_from("<3H", blob, anim["samples"]["offset"] + frame * anim["node_count"] * 12)
        positions.append([v * anim["sample_position_scale"] + anim["sample_position_min"] for v in q])
    speed = math.hypot(positions[1][0] - positions[0][0], positions[1][2] - positions[0][2]) * anim["fps"] / (anim["frame_count"] - 1)
    # Derive eye height from the exact replay foot poses instead of assuming 180 cm.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from formats import read_rays
    import numpy as np
    rays = read_rays(inputs / "rays.bin")
    poses = [list(map(float, line.split()[2:5])) for line in (inputs / "poses.txt").read_text().splitlines() if line.strip() and not line.startswith("#")]
    assert len(poses) == len(rays.updates)
    offsets = rays.updates["origin"].astype(float) - np.array(poses)
    eye_height = float(np.median(offsets[:, 1]))
    timing = json.loads((inputs / "perception-timing.json").read_text())
    slope, step = timing["max_slope_degrees"], timing["step_height_cm"]
    (raw / "character.txt").write_text(" ".join(map(str, (radius, height, step, slope, speed, eye_height))) + "\n")
    write(raw / "character-input.json", {
        "base_fingerprint": fingerprint, "hierarchy_sha256": hashlib.sha256(hierarchy.read_bytes()).hexdigest(),
        "collision_bbox_cm": [low, high], "radius_cm": radius, "height_cm": height,
        "step_height_cm": step, "max_slope_degrees": slope, "run_speed_cm_s": speed,
        "speed_source": "HUMANS-S_RUNL.MAN root displacement / elapsed frames; no animation overlays",
        "eye_height_cm": eye_height, "eye_height_min_max_cm": [float(offsets[:, 1].min()), float(offsets[:, 1].max())],
        "head_xz_offset_max_cm": float(np.hypot(offsets[:, 0], offsets[:, 2]).max()),
    })


def gpu_skip(raw, phase):
    log = raw / ("warp-install.log" if phase == "install" else "warp.log")
    write(raw / "gpu.json", {"status": "skipped", "phase": phase, "reason": log.read_text()[-6000:]})


def summarize(inputs, raw, job_class):
    import numpy as np
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from formats import KINDS, read_rays
    rows = [json.loads(line) for line in (raw / "cpu.jsonl").read_text().splitlines()]
    def stats(values):
        return {"median": float(np.median(values)), "p90": float(np.percentile(values, 90)), "repeats": len(values)}
    result = {"status": "needs_decision", "raw_root": str(raw), "metadata": next(r for r in rows if r["type"] == "metadata"),
              "category_to_kind": {"1": "World", "2": "StaticVob", "3": "Water", "4": "StaticVob"},
              "open_items": [
                  "Numerical attribution of replay/runner eye gap needs the original body boundaries/insertion layout, absent from NAVSTAT1.",
                  "Twenty static mismatch examples are recorded, but exact engine source-face/edge causes cannot all be isolated without source metadata."
              ],
              "hardware": {"cpu": (raw / "cpu.txt").read_text(), "gpu_driver": (raw / "gpu.txt").read_text()},
              "measurement": {"class": job_class, "gq_status": (raw / "gq-status-run.txt").read_text(),
                              "loadavg_at_start": (raw / "load-run.txt").read_text(), "started_utc": (raw / "start-run.txt").read_text().strip(),
                              "repeats": 3, "warmup": "One full CPU replay per backend; one full replay per thread count; 30 CV frames; 5 GPU launches",
                              "cast_definition": "Amortized eye time / actual near+far casts; includes packet staging, normal lookup and result conversion; excludes head pose, direction generation and engine source-plane refinement",
                              "flags": "-O3 -DNDEBUG -march=x86-64-v3 -ffp-contract=off; Jolt SIMD defaults, LTO/profiler/debug renderer disabled",
                              "input_sha256": (raw / "input.sha256").read_text(),
                              "embree_release": "https://github.com/RenderKit/embree/releases/tag/v4.4.0",
                              "embree_archive_sha256": (raw / "embree-archive.sha256").read_text().split()[0]},
              "bvh_build_us": {backend: stats([r["us"] for r in rows if r["type"] == "build" and r["backend"] == backend]) for backend in ("jolt", "embree")},
              "optimize_broadphase_us": stats([r["us"] for r in rows if r["type"] == "optimize"]),
              "bvh_build_warmup": "Three fresh scenes per backend; no warmup; Jolt mesh preprocessing/body insertion included; Embree geometry buffers included",
              "single_thread": {}, "throughput": {}, "agreement": {}, "character": [],
              "gpu": json.loads((raw / "gpu.json").read_text()),
              "research": {
                  "reference": "238 eye rays; near 1000cm/100ms, far after near miss every 500ms; NAVSTAT1 2838834 triangles in cm; acceptance uses exact endpoint replay and synthetic box 0.001cm",
                  "embree": "https://github.com/RenderKit/embree/blob/v4.4.0/README.md",
                  "warp": "https://github.com/NVIDIA/warp/blob/main/warp/native/mesh.h",
                  "pitfalls": "Embree argument filters must be enabled on scene/geometry; Warp mesh_query_ray has no native backface filter; static dump lacks body/subshape and interactive metadata",
                  "engine_support": "Vendored Jolt 5.6.0 provides no-lock queries and native CharacterVirtual; PhysicsSystem::Update rebuilds dirty broadphase trees without an explicit OptimizeBroadPhase call"},
              "limitations": [
                  "NAVSTAT1 has no source body IDs: Jolt uses one static mesh body per category. OptimizeBroadPhase on four bodies cannot reproduce the engine's 6993-body insertion tree.",
                  "Static snapshot excludes NPCs and movable props; category 4 is frozen in its dumped kinematic pose.",
                  "Dumped Jolt triangles are quantized; source-plane refinement, edge retry and source duplicate/material metadata are absent.",
                  "CharacterVirtual excludes engine contact listeners, interactive furniture stair veto, NPC clipping, moving supports and world stepping. Measures native static collision cost, not the full MoveAlgo wrapper.",
              ]}
    groups = defaultdict(list)
    for row in rows:
        if row["type"] in ("single", "throughput"):
            groups[(row["type"], row["backend"], row.get("threads", 1))].append(row)
    for (typ, backend, threads), group in groups.items():
        if typ == "single":
            result["single_thread"][backend] = {key: {quant: stats([g[key][quant] for g in group]) for quant in ("median", "p90")} for key in ("eye_us", "near_only_us", "full_us", "cast_us", "near_only_cast_us", "full_cast_us")}
            result["single_thread"][backend].update(casts=group[0]["casts"], updates=group[0]["updates"], cull=bool(group[0]["cull"]))
        else:
            result["throughput"].setdefault(backend, {})[str(threads)] = {key: stats([g[key] for g in group]) for key in ("updates_per_second", "casts_per_second", "eye_rays_per_second")}
    result["embree_backfaces"] = "RTCIntersectArguments.filter + RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS; reject dot(direction, Ng) >= 0; geometry argument-filter enabled; two-sided queries omit filter. Incoherent traversal."
    rays = read_rays(inputs / "rays.bin")
    ref = rays.updates["rays"]
    dtype = np.dtype([("distance", "<f4"), ("normal", "<f4"), ("kind", "<u4"), ("flags", "<u4")])
    predictions = {}
    for path in sorted(raw.glob("*.hits")):
        predictions[path.stem] = np.memmap(path, mode="r", dtype=dtype, shape=ref.shape)
    for backend, pred in predictions.items():
        same_hit = ((pred["flags"] & 1) != 0) == ref["hit"]
        distance = np.abs(pred["distance"] - ref["distance_cm"])
        normal = np.abs(pred["normal"] - ref["normal_y"])
        same_kind = pred["kind"] == ref["kind"]
        both_hit = ((pred["flags"] & 1) != 0) & ref["hit"].astype(bool)
        strict = same_hit & (distance < 1) & same_kind & (normal < .01)
        result["agreement"][backend] = {}
        for k, name in enumerate(KINDS):
            sel = ref["kind"] == k
            n = int(sel.sum())
            def fraction(mask):
                return float(mask[sel].mean()) if n else None
            result["agreement"][backend][name] = {"rays": n, "hit_equal_fraction": fraction(same_hit), "distance_lt_1cm_fraction": fraction(distance < 1),
                                                  "kind_equal_fraction": fraction(same_kind), "normal_y_lt_0_01_fraction": fraction(normal < .01), "all_fraction": fraction(strict),
                                                  "both_hit_rays": int((both_hit & sel).sum())}
        result["agreement"][backend]["all"] = {"rays": strict.size, "all_fraction": float(strict.mean()), "complete_equal_fraction": float((((pred["flags"] >> 1) & 1) == ref["complete"]).mean())}
    # Interleave examples across the three static engine kinds and the two backends.
    examples = []
    for backend in ("jolt-optimized", "embree1-cull"):
        pred = predictions[backend]
        bad = (((pred["flags"] & 1) != ref["hit"]) | (np.abs(pred["distance"] - ref["distance_cm"]) >= 1) |
               (pred["kind"] != ref["kind"]) | (np.abs(pred["normal"] - ref["normal_y"]) >= .01))
        for k in (1, 2, 5):
            indices = np.argwhere(bad & (ref["kind"] == k))
            # Spatially spread the diagnostics rather than taking one trajectory's edge repeatedly.
            chosen = indices[np.linspace(0, len(indices)-1, min(10, len(indices)), dtype=int)] if len(indices) else []
            for u, r in chosen:
                p = pred[u, r]; e = ref[u, r]
                two = predictions["embree1-two-sided"][u, r]
                reason = "Unresolved from NAVSTAT1: source planes, original body/subshape IDs and edge retry metadata are not exported."
                if backend.startswith("jolt") and abs(float(p["distance"])-float(predictions["embree1-cull"][u, r]["distance"])) >= 1:
                    reason = "Jolt rebuilt mesh differs from Embree on the same dumped triangle soup; mesh re-quantization/edge tolerance are candidate causes, not isolated."
                if not (p["flags"] & 1) and (two["flags"] & 1):
                    reason = "Backface sensitivity confirmed: two-sided Embree finds geometry rejected by the front-face query. Engine's selected source face is unavailable."
                examples.append({"backend": backend, "update": int(u), "ray": int(r), "engine_kind": KINDS[k],
                                 "origin_cm": rays.updates[u]["origin"].tolist(), "direction": e["direction"].tolist(), "far_updated": bool(rays.updates[u]["far_updated"]),
                                 "engine_distance_cm": float(e["distance_cm"]), "backend_distance_cm": float(p["distance"]),
                                 "engine_normal_y": float(e["normal_y"]), "backend_normal_y": float(p["normal"]),
                                 "backend_kind": KINDS[int(p["kind"])], "reason": reason})
    by_case = defaultdict(list)
    for example in examples:
        by_case[(example["backend"], example["engine_kind"])].append(example)
    examples = [group[i] for i in range(10) for group in by_case.values() if i < len(group)][:20]
    write(raw / "mismatches.json", examples)
    result["mismatch_examples"] = examples[:20]
    engine = json.loads((inputs / "engine.json").read_text())
    memory = json.loads((inputs.parent / "memory/results.json").read_text())
    runner = next(p for p in memory["population"] if p["N"] == 50 and p["runners"] == 1 and p["weight"] == "hd")
    result["engine_comparison"] = {"replay_us_per_update": engine["eye_us_per_update"], "replay_us_per_cast": engine["eye_us_per_cast"],
        "runner_n50_hd_us_per_decision": runner["profile_us"]["eye"],
        "replay_to_runner_median_ratio": engine["eye_us_per_update"]["eye"]["median_us"] / runner["profile_us"]["eye"]["median"],
        "jolt_added_to_optimized_median_ratio": result["single_thread"]["jolt-added"]["eye_us"]["median"]["median"] / result["single_thread"]["jolt-optimized"]["eye_us"]["median"]["median"],
        "interpretation": "The measured runner cost is authoritative for that N=50 HD workload; replay is a different unstepped world. PhysicsSystem::Update rebuilds dirty broadphase trees through BroadPhaseQuadTree::UpdatePrepare even without OptimizeBroadPhase. Replay also times navigationEyePose and source-face refinements. Their separate numerical contributions cannot be isolated from NAVSTAT1's four-category mesh layout; no causal speedup is claimed.",
        "code": ["OpenGothic/common/game/navperceptionbench.cpp:390", "OpenGothic/common/game/navigationeye.cpp:146", "OpenGothic/common/navdemos.cpp:1051", "OpenGothic/lib/JoltPhysics/Jolt/Physics/PhysicsSystem.cpp:180", "OpenGothic/lib/JoltPhysics/Jolt/Physics/Collision/BroadPhase/BroadPhaseQuadTree.cpp:106"]}
    result["character_input"] = json.loads((raw / "character-input.json").read_text())
    result["character_setup"] = next(r for r in rows if r["type"] == "character_setup")
    for count in (1, 100, 1000):
        for threads in sorted({r["threads"] for r in rows if r["type"] == "character"}):
            group = [r for r in rows if r["type"] == "character" and r["count"] == count and r["threads"] == threads]
            result["character"].append({"count": count, "threads": threads, "active_workers": min(count, threads), "hz": 30, "warmup_frames": 30, "measured_frames": 90,
                                        **{key: stats([r[key] for r in group]) for key in ("wall_us_per_character", "character_updates_per_second")}})
    write(raw / "standalone.json", result)


if __name__ == "__main__":
    command = sys.argv[1]
    if command == "prepare":
        prepare(Path(sys.argv[2]), Path(sys.argv[3]))
    elif command == "gpu-skip":
        gpu_skip(Path(sys.argv[2]), sys.argv[3])
    elif command == "summarize":
        summarize(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4])
    else:
        raise ValueError(command)
