#!/usr/bin/env python3
"""Navigation memory on the GPU: parity with the engine and cost per agent.

One command from the Mac stages everything on dev-ubuntu, runs one gq job and
collects the report::

    python3 scripts/navsim/gpumem/bench.py remote --agents 64,256,1024,2048

Larger N needs more VRAM than the shared GPU leaves (about 1.73 MB per agent
with the readout batch). With the services stopped, the scale part alone::

    python3 scripts/navsim/gpumem/bench.py remote --agents 4096,8192 --skip-parity \
        --thread-per-agent "" --float-confidence "" --gpu-mem 20 --vram-limit-gib 19

Each run appends to ``runs`` in the report; parity is kept from the last run
that had it.

Commands inside the job:
  parity  every readout of every capture through the engine (navsim_memory_ref,
          ctypes, built from this checkout) and the batched port, bit for bit;
          the HD net's argmax actions on both inputs, each with its own GRU state
  scale   per-agent update/readout cost and VRAM for a list of N, one process
          per N; captures repeat across agents with shifted starts
"""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import captures  # noqa: E402
import gpumem  # noqa: E402

ROOT = HERE.parents[2]
MEMORY_REPORT = ROOT / "reports/navigation/2026-11-04-nav-sim-research/data/memory/results.json"
REPORT = ROOT / "reports/navigation/2026-11-04-nav-sim-research/data/gpumem/results.json"
NAS = "/mnt/shared/gothic-ai/nav-sim-research/gpumem"
WEIGHTS = "/mnt/shared/gothic-ai/nav-pace-v5/run"
REMOTE_TASK = "gothic-ai/nav-sim-research/gpumem"
HISTORY = 1024
STEPS_PER_REPEAT = 30
REFERENCE_FLAGS = ["-O2", "-DNDEBUG", "-std=gnu++20", "-fPIC", "-shared"]
REFERENCE_SOURCES = ["tests/navsim_memory_ref.cpp", "common/navpolicy.cpp", "common/game/navigationmemory.cpp"]
STAGED = (["scripts/navsim/gpumem/" + n for n in ("bench.py", "gpumem.py", "captures.py", "memory_kernels.h",
                                                  "memory_cuda.cu", "memory_host.cpp")]
          + ["scripts/nav_bc_model.py"]
          + ["OpenGothic/" + n for n in REFERENCE_SOURCES[1:] + ["tests/navsim_memory_ref.cpp"]]
          + ["OpenGothic/common/" + n for n in ("navpolicy.h", "navdemowriter.h", "navmemorycosttrace.h",
                                                "game/navigationeye.h", "game/navigationmemory.h",
                                                "game/npckeys.h")]
          + ["OpenGothic/lib/Tempest/Engine/include/Tempest/Vec", "OpenGothic/lib/Tempest/Engine/utility/utility.h"])


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=1, allow_nan=False) + "\n")


def sha256(path):
    with open(path, "rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def stats(values):
    values = np.asarray(values, np.float64)
    return dict(median=float(np.median(values)), p90=float(np.percentile(values, 90)), repeats=int(values.size))


def nvidia(*fields):
    out = subprocess.check_output(["nvidia-smi", f"--query-gpu={','.join(fields)}", "--format=csv,noheader,nounits"],
                                  text=True)
    return [v.strip() for v in out.strip().splitlines()[0].split(",")]


def process_vram_mib():
    out = subprocess.check_output(["nvidia-smi", "--query-compute-apps=pid,used_memory",
                                   "--format=csv,noheader,nounits"], text=True)
    for line in out.splitlines():
        pid, used = (v.strip() for v in line.split(","))
        if int(pid) == os.getpid():
            return int(used)
    return None


# ---- engine reference (ctypes) ----
class Reference:
    library = None

    def __init__(self, library, capture, weights, journey=None):
        if Reference.library is None:
            lib = ctypes.CDLL(str(library))
            lib.navref_open.restype = ctypes.c_void_p
            lib.navref_open.argtypes = [ctypes.c_char_p] * 3
            lib.navref_error.restype = ctypes.c_char_p
            lib.navref_close.argtypes = [ctypes.c_void_p]
            lib.navref_sizes.restype = ctypes.c_int64
            lib.navref_sizes.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_int64)] * 3
            lib.navref_journey.argtypes = [ctypes.c_void_p]
            lib.navref_next.argtypes = [ctypes.c_void_p] * 4 + [ctypes.POINTER(ctypes.c_uint64)]
            lib.navref_tanf.restype = ctypes.c_float
            lib.navref_tanf.argtypes = [ctypes.c_float]
            Reference.library = lib
        lib = Reference.library
        self.handle = lib.navref_open(str(capture).encode(), str(weights).encode(),
                                      None if journey is None else str(journey).encode())
        if not self.handle:
            raise RuntimeError(lib.navref_error().decode())
        sizes = [ctypes.c_int64() for _ in range(3)]
        self.weight_version = lib.navref_sizes(self.handle, *[ctypes.byref(s) for s in sizes])
        if [s.value for s in sizes[:2]] != [10 * 64 * 64, 10 * 50 * 100]:
            raise RuntimeError("reference weights are not HD format")
        self.near = np.empty((10, 64, 64), np.float32)
        self.far = np.empty((10, 50, 100), np.float32)
        self.vector = np.empty(sizes[2].value, np.float32)
        self.journey = lib.navref_journey(self.handle)
        self.updates = ctypes.c_uint64()

    def next(self):
        result = Reference.library.navref_next(self.handle, self.near.ctypes.data, self.far.ctypes.data,
                                               self.vector.ctypes.data, ctypes.byref(self.updates))
        if result < 0:
            raise RuntimeError(Reference.library.navref_error().decode())
        return result == 1

    def close(self):
        Reference.library.navref_close(self.handle)


def build_reference(source_root, output):
    """Same flags as gothic-navsim-memory-bench (no -march, no FP contraction on x86-64)."""
    root = Path(source_root) / "OpenGothic"
    command = ["g++", *REFERENCE_FLAGS, f"-I{root}/common", f"-I{root}/lib/Tempest/Engine/include",
               f"-I{root}/lib/Tempest/Engine", f"-I{root}/lib/rapidjson/include",
               *[str(root / s) for s in REFERENCE_SOURCES], "-o", str(output)]
    subprocess.run(command, check=True)
    return " ".join(command)


# ---- device helpers ----
def device_ints(backend, values):
    values = np.ascontiguousarray(values, np.int32)
    if backend.kind == "cuda":
        return backend.torch.from_numpy(values).to("cuda")
    return values


def device_floats(backend, shape):
    if backend.kind == "cuda":
        return backend.torch.zeros(shape, dtype=backend.torch.float32, device="cuda")
    return np.zeros(shape, np.float32)


def host(backend, value):
    return value.cpu().numpy() if backend.kind == "cuda" else value


class Policy:
    """HD net (format 5) on CUDA, fp32 without TF32; argmax of every head."""

    def __init__(self, torch, weights):
        sys.path.insert(0, str(HERE.parents[1]))
        from nav_bc_model import NavPolicy
        state = torch.load(weights, map_location="cpu", weights_only=False)
        config = {**state["config"], "auxiliary": False}
        self.model = NavPolicy(**config)
        self.model.load_state_dict({k: v for k, v in state["model"].items() if not k.startswith("aux_")})
        self.model = self.model.to("cuda").eval()
        self.torch = torch
        self.heads = list(self.model.head_sizes)

    def act(self, near, far, vector, hidden):
        with self.torch.inference_mode():
            logits, hidden = self.model(near[:, None], far[:, None], vector[:, None], hidden=hidden)
        return self.torch.stack([logits[h][:, 0].argmax(-1) for h in self.heads], 1), hidden


class Comparison:
    def __init__(self):
        self.readouts = self.values = self.differing = self.unequal_readouts = 0
        self.max_abs = 0.0
        self.min_equal_fraction = 1.0
        self.first = None
        self.channels = {"near": [0] * 10, "far": [0] * 10}

    def add(self, name, readout, actual, expected):
        bits = actual.view(np.uint32) != expected.view(np.uint32)
        count = int(bits.sum())
        if count:
            delta = np.abs(actual.astype(np.float64) - expected.astype(np.float64))[bits]
            self.max_abs = max(self.max_abs, float(np.nan_to_num(delta, nan=np.inf).max()))
            for channel, n in enumerate(bits.reshape(10, -1).sum(1)):
                self.channels[name][channel] += int(n)
            if self.first is None:
                c, row, col = (int(v) for v in np.argwhere(bits)[0])
                self.first = dict(readout=readout, plane=name, channel=c, row=row, column=col,
                                  gpu=float(actual[c, row, col]), engine=float(expected[c, row, col]))
        return count

    def readout(self, near, far):
        self.readouts += 1
        values = near + far
        self.values += values
        return values


def run_parity(backend, records, library, weights, journey=None, lanes=32, policy=None, label=""):
    references = [Reference(library, c.path, weights, journey) for c in records]
    N, R = len(records), records[0].slots
    if any(c.slots != R for c in records):
        raise ValueError("captures differ in eye slots")
    maps, ids = [], []
    for c in records:
        ids.append(len(maps) if len(c.tiles) else -1)
        if len(c.tiles):
            maps.append((c.tiles, c.mask))
    memory = gpumem.Memory(backend, N, R, HISTORY, [r.journey for r in references], maps, ids)
    table = gpumem.EyeTable(backend, records)
    near_out, far_out = device_floats(backend, (N, 10, 64, 64)), device_floats(backend, (N, 10, 50, 100))
    per = [Comparison() for _ in records]
    actions = [dict(readouts=0, equal=0, first=None) for _ in records]
    hidden = None
    if policy is not None:
        torch = policy.torch
        hidden = {k: torch.zeros(1, N, 256, device="cuda") for k in ("engine", "gpu")}
    started = time.perf_counter()
    for step in range(max(c.updates for c in records)):
        updates = np.array([table.first[a] + step if step < records[a].updates else -1 for a in range(N)], np.int32)
        memory.scatter(table, device_ints(backend, updates))
        memory.update(lanes)
        readers = [a for a in range(N) if updates[a] >= 0 and table.views_after[updates[a]] > 0]
        if not readers:
            continue
        memory.read(device_ints(backend, readers), near_out, far_out)
        near, far = host(backend, near_out), host(backend, far_out)
        batch = dict(engine=([], [], []), gpu=([], [], []))
        for k, a in enumerate(readers):
            for _ in range(int(table.views_after[updates[a]])):
                if not references[a].next():
                    raise RuntimeError(f"{records[a].path}: engine has fewer readouts")
                ref = references[a]
                index = per[a].readouts
                differing = per[a].add("near", index, near[k], ref.near) + per[a].add("far", index, far[k], ref.far)
                values = per[a].readout(near[k].size, far[k].size)
                per[a].differing += differing
                per[a].unequal_readouts += differing > 0
                per[a].min_equal_fraction = min(per[a].min_equal_fraction, 1 - differing / values)
            if policy is not None:
                for key, maps_ in (("engine", (references[a].near, references[a].far)), ("gpu", (near[k], far[k]))):
                    batch[key][0].append(maps_[0])
                    batch[key][1].append(maps_[1])
                    batch[key][2].append(references[a].vector)
        if policy is not None:
            torch = policy.torch
            index = torch.tensor(readers, device="cuda")
            chosen = {}
            for key, (n, f, v) in batch.items():
                inputs = [torch.from_numpy(np.stack(x)).to("cuda") for x in (n, f, v)]
                chosen[key], new = policy.act(*inputs, hidden[key][:, index].contiguous())
                hidden[key][:, index] = new
            same = (chosen["engine"] == chosen["gpu"]).all(1).cpu().numpy()
            for k, a in enumerate(readers):
                actions[a]["readouts"] += 1
                actions[a]["equal"] += bool(same[k])
                if not same[k] and actions[a]["first"] is None:
                    actions[a]["first"] = dict(readout=per[a].readouts - 1,
                                               engine=chosen["engine"][k].tolist(), gpu=chosen["gpu"][k].tolist())
    elapsed = time.perf_counter() - started
    for r, c in zip(references, records):
        if r.next():
            raise RuntimeError(f"{c.path}: engine has more readouts")
        r.close()
    errors = memory.get("errors")
    out = []
    for c, p, act in zip(records, per, actions):
        item = dict(capture=c.path, updates=c.updates, readouts=p.readouts, values=p.values,
                    bitwise_equal_values=p.values - p.differing, differing_values=p.differing,
                    bitwise_equal_fraction=1 - p.differing / max(1, p.values),
                    min_readout_equal_fraction=p.min_equal_fraction, unequal_readouts=p.unequal_readouts,
                    max_abs_delta=p.max_abs, first_mismatch=p.first, differing_by_channel=p.channels)
        if policy is not None:
            item["actions"] = act
        out.append(item)
    total = dict(label=label, captures=len(records), lanes=lanes, journey=[r.journey for r in references],
                 readouts=sum(p.readouts for p in per), updates=int(sum(c.updates for c in records)),
                 values=sum(p.values for p in per), differing_values=sum(p.differing for p in per),
                 unequal_readouts=sum(p.unequal_readouts for p in per),
                 max_abs_delta=max(p.max_abs for p in per),
                 min_readout_equal_fraction=min(p.min_equal_fraction for p in per),
                 first_mismatch=next(({"capture": c.path, **p.first} for c, p in zip(records, per) if p.first), None),
                 state_errors=int(np.bitwise_or.reduce(errors)) if len(errors) else 0, seconds=elapsed)
    total["bitwise_equal_fraction"] = 1 - total["differing_values"] / max(1, total["values"])
    if policy is not None:
        total["actions"] = dict(readouts=sum(a["readouts"] for a in actions), equal=sum(a["equal"] for a in actions),
                                heads=policy.heads)
        total["actions"]["equal_fraction"] = total["actions"]["equal"] / max(1, total["actions"]["readouts"])
    return dict(total=total, captures=out)


def parity_command(args):
    backend = gpumem.Backend(args.backend, args.build, args.variant)
    records = [captures.read(p) for p in args.captures]
    policy = None
    if args.policy:
        import torch
        torch.backends.cuda.matmul.allow_tf32 = False
        torch.backends.cudnn.allow_tf32 = False
        torch.backends.cudnn.benchmark = False
        torch.backends.cudnn.deterministic = True
        policy = Policy(torch, args.policy)
    report = dict(variant=args.variant, records=run_parity(backend, records, args.reference, args.weights, lanes=args.lanes,
                                     policy=policy, label="drive captures"))
    synthetic = []
    for seed, journey in ((1, "0"), (2, "1")):
        path = Path(args.build) / f"synthetic-{seed}.bin"
        captures.write(path, captures.synthetic(seed))
        result = run_parity(backend, [captures.read(path)], args.reference, args.weights, journey=journey,
                            lanes=args.lanes, label=f"synthetic seed {seed} journey {journey}")
        synthetic.append(dict(sha256=sha256(path), **result))
    report["synthetic"] = synthetic
    slope = np.float32(np.float32(np.float32(49.999996) * np.float32(math.pi)) / np.float32(180))
    engine_tan = Reference.library.navref_tanf(float(slope))
    report["slope_tan"] = dict(engine_tanf=engine_tan, device_double_tan=float(np.float32(math.tan(float(slope)))),
                               equal=engine_tan == float(np.float32(math.tan(float(slope)))))
    write_json(args.output, report)
    print(json.dumps(report["records"]["total"]), flush=True)


# ---- scale ----
def schedule(records, table, N, steps):
    C = len(records)
    record = np.arange(N) % C
    phase = np.mod((np.arange(N) // C) * 0.6180339887498949 + record * 0.3819660112501051, 1.0)
    starts = np.array([int(p * records[r].updates) for p, r in zip(phase, record)], np.int64)
    lengths = np.array([records[r].updates for r in record], np.int64)
    local = (starts[None, :] + np.arange(steps)[:, None]) % lengths[None, :]
    updates = (table.first[record][None, :] + local).astype(np.int32)
    for c in records:
        if c.begin[1] - c.begin[0] != c.slots:
            raise ValueError(f"{c.path}: first update does not refresh every slot; wrap-around needs it")
    return record, starts, updates


def cell_command(args):
    import torch
    torch.backends.cuda.matmul.allow_tf32 = False
    total_vram = torch.cuda.get_device_properties(0).total_memory
    if args.vram_limit_gib:
        torch.cuda.set_per_process_memory_fraction(min(1.0, args.vram_limit_gib * 2 ** 30 / total_vram))
    free_before = int(nvidia("memory.free")[0])
    result = dict(agents=args.agents, lanes=args.lanes, variant=args.variant, steps_per_repeat=STEPS_PER_REPEAT,
                  warmup=args.warmup, repeats=args.repeats, free_vram_mib_before=free_before,
                  vram_limit_gib=args.vram_limit_gib, status="ok")
    try:
        backend = gpumem.Backend("cuda", args.build, args.variant)
        torch.cuda.reset_peak_memory_stats()
        records = [captures.read(p) for p in args.captures]
        N, R = args.agents, records[0].slots
        table = gpumem.EyeTable(backend, records)
        steps = (args.warmup + args.repeats) * STEPS_PER_REPEAT
        record, starts, updates = schedule(records, table, N, steps)
        memory = gpumem.Memory(backend, N, R, HISTORY, journey=0)
        cache, rays = {}, np.zeros((N, R), captures.RAY)
        for a in range(N):
            key = (int(record[a]), int(starts[a]))
            if key not in cache:
                cache[key] = records[key[0]].slots_after(key[1] - 1)
            rays[a] = cache[key]
        memory.load_slots(rays)
        del rays, cache
        readers = [np.flatnonzero(table.views_after[updates[s]] > 0).astype(np.int32) for s in range(steps)]
        widest = max(1, max(len(r) for r in readers))
        padded = np.zeros((steps, widest), np.int32)
        for s, r in enumerate(readers):
            padded[s, : len(r)] = r
        updates_gpu = torch.from_numpy(updates).to("cuda")
        readers_gpu = torch.from_numpy(padded).to("cuda")
        near = torch.zeros(widest, 10, 64, 64, device="cuda")
        far = torch.zeros(widest, 10, 50, 100, device="cuda")
        events = [[torch.cuda.Event(enable_timing=True) for _ in range(3)] for _ in range(STEPS_PER_REPEAT)]
        update_us, read_us, rate, readout_rate, update_ms_raw, read_ms_raw = [], [], [], [], [], []
        for repeat in range(args.warmup + args.repeats):
            reads = 0
            for i in range(STEPS_PER_REPEAT):
                s = repeat * STEPS_PER_REPEAT + i
                memory.scatter(table, updates_gpu[s])
                events[i][0].record()
                memory.update(args.lanes)
                events[i][1].record()
                memory.read(readers_gpu[s, : len(readers[s])], near, far)
                events[i][2].record()
                reads += len(readers[s])
            torch.cuda.synchronize()
            if repeat < args.warmup:
                continue
            up = sum(e[0].elapsed_time(e[1]) for e in events)
            rd = sum(e[1].elapsed_time(e[2]) for e in events)
            update_ms_raw.append(up)
            read_ms_raw.append(rd)
            update_us.append(up * 1000 / (STEPS_PER_REPEAT * N))
            read_us.append(rd * 1000 / max(1, reads))
            rate.append(STEPS_PER_REPEAT * N / (up / 1000))
            readout_rate.append(reads / (rd / 1000))
        errors = memory.get("errors")
        result.update(
            update_us_per_agent=stats(update_us), readout_us_per_agent=stats(read_us),
            agent_updates_per_second=stats(rate), agent_readouts_per_second=stats(readout_rate),
            readouts_per_update=float(np.mean([len(r) for r in readers]) / N),
            memory_us_per_decision=float(np.median(update_us)) * float(N * steps / sum(len(r) for r in readers))
            + float(np.median(read_us)),
            update_ms_per_repeat=update_ms_raw, readout_ms_per_repeat=read_ms_raw,
            peak_allocated_bytes=torch.cuda.max_memory_allocated(),
            peak_reserved_bytes=torch.cuda.max_memory_reserved(),
            process_vram_mib=process_vram_mib(), free_vram_mib_after=int(nvidia("memory.free")[0]),
            bytes_per_agent=memory.bytes_per_agent(), eye_table_bytes=table.device_bytes,
            readout_batch=widest, readout_buffer_bytes=4 * widest * 10 * gpumem.OUTPUTS,
            state_errors=int(np.bitwise_or.reduce(errors)),
            resets=int(sum(int(np.count_nonzero(np.diff(updates[:, a]) < 0)) for a in range(N))))
    except torch.cuda.OutOfMemoryError as error:
        result.update(status="out_of_memory", error=str(error).splitlines()[0],
                      peak_allocated_bytes=torch.cuda.max_memory_allocated())
    write_json(args.output, result)


def scale_command(args):
    cells = []
    plan = ([(n, 32, "exact") for n in args.agents] + [(n, 1, "exact") for n in args.thread_per_agent]
            + [(n, 32, "float-confidence") for n in args.float_confidence])
    for n, lanes, variant in plan:
        output = Path(args.output).with_name(f"cell-n{n}-l{lanes}-{variant}.json")
        command = [sys.executable, str(Path(__file__).resolve()), "cell", "--agents", str(n), "--lanes", str(lanes),
                   "--variant", variant,
                   "--build", str(args.build), "--output", str(output), "--warmup", str(args.warmup),
                   "--repeats", str(args.repeats), "--vram-limit-gib", str(args.vram_limit_gib), "--captures",
                   *args.captures]
        print(f"[scale] N={n} lanes={lanes} {variant}", flush=True)
        run = subprocess.run(command)
        cell = json.loads(output.read_text()) if output.exists() else dict(agents=n, lanes=lanes, variant=variant,
                                                                         status="crashed", exit_code=run.returncode)
        cells.append(cell)
        print(f"  {cell.get('status')} update {cell.get('update_us_per_agent')} read {cell.get('readout_us_per_agent')}",
              flush=True)
        if cell.get("status") != "ok" and lanes == 32 and variant == "exact":
            break  # larger N cannot fit either
    write_json(args.output, dict(cells=cells))


def metadata_command(args):
    import torch
    info = dict(gpu=nvidia("name", "driver_version", "memory.total"), torch=torch.__version__,
                torch_cuda=torch.version.cuda, cudnn=torch.backends.cudnn.version(), python=sys.version,
                nvcc=subprocess.check_output(["nvcc", "--version"], text=True).strip().splitlines()[-2:],
                gcc=subprocess.check_output(["g++", "--version"], text=True).splitlines()[0],
                glibc=subprocess.check_output(["ldd", "--version"], text=True).splitlines()[0],
                cuda_flags=gpumem.CUDA_FLAGS, compute_capability=list(torch.cuda.get_device_capability(0)),
                free_vram_mib=int(nvidia("memory.free")[0]), utc=datetime.now(timezone.utc).isoformat())
    write_json(args.output, info)


# ---- launcher (Mac) ----
def ssh(host, script, check=True):
    return subprocess.run(["ssh", host, "bash -s"], input=script, text=True, capture_output=True, check=check)


def remote_command(args):
    memory = json.loads(MEMORY_REPORT.read_text())
    traces = memory["traces"]
    host = "dev-ubuntu"
    home = ssh(host, 'printf %s "$HOME"').stdout
    task = f"{home}/{REMOTE_TASK}"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run = f"{task}/runs/{stamp}"
    print(ssh(host, f"df -h ~ | tail -1; mkdir -p {task}/src {task}/captures {task}/weights {run}").stdout, flush=True)
    archive = Path("/tmp") / f"navsim-gpumem-src-{stamp}.tar"
    subprocess.run(["tar", "-cf", str(archive), "-C", str(ROOT), *STAGED, "OpenGothic/lib/rapidjson/include"], check=True)
    subprocess.run(["scp", "-q", str(archive), f"{host}:{task}/src.tar"], check=True)
    archive.unlink()
    ssh(host, f"rm -rf {task}/src && mkdir -p {task}/src && tar -xf {task}/src.tar -C {task}/src && rm {task}/src.tar")
    # Captures and weights come from Ryzen's NAS through the Mac; SHA-256 is checked on arrival.
    wanted = [(t["path"], f"{task}/captures/{Path(t['path']).parents[1].name}.bin", t["sha256"]) for t in traces]
    weights = [(f"{WEIGHTS}/policy.navbc", f"{task}/weights/policy.navbc", memory["weights"]["v5"]["sha256"]),
               (f"{WEIGHTS}/last.pt", f"{task}/weights/last.pt", None)]
    for source, target, digest in wanted + weights:
        present = ssh(host, f"test -f {target} && sha256sum {target} | cut -d' ' -f1", check=False).stdout.strip()
        if present and (digest is None or present == digest):
            continue
        subprocess.run(["scp", "-q", "-3", f"ryzen:{source}", f"{host}:{target}"], check=True)
        got = ssh(host, f"sha256sum {target} | cut -d' ' -f1").stdout.strip()
        if digest and got != digest:
            raise RuntimeError(f"{target}: sha256 {got} != {digest}")
    capture_list = " ".join(shlex.quote(t) for _, t, _ in wanted)
    agents = ",".join(str(n) for n in args.agents)
    single = ",".join(str(n) for n in args.thread_per_agent)
    fast = ",".join(str(n) for n in args.float_confidence)
    parity = "" if args.skip_parity else "".join(f"""
$py src/scripts/navsim/gpumem/bench.py parity --backend cuda --variant {variant} --build {run}/build \\
  --reference {run}/build/libnavmemref.so --weights weights/policy.navbc --policy weights/last.pt \\
  --output {run}/parity{suffix}.json --captures {capture_list}
""" for variant, suffix in (("exact", ""), ("float-confidence", "-float-confidence")) if variant == "exact" or fast)
    body = f"""set -eu
cd {task}
df -h ~ | tail -1
nvidia-smi --query-gpu=memory.used,memory.free --format=csv
export UV_LINK_MODE=hardlink CUDA_HOME=/usr/local/cuda PATH=/usr/local/cuda/bin:$PATH
if [ ! -x .venv/bin/python ]; then ~/.local/bin/uv venv -q --python 3.12 .venv; fi
# Offline: every wheel comes from the uv cache as hard links, nothing is downloaded.
~/.local/bin/uv pip install -q --offline --python .venv/bin/python numpy ninja 'torch=={args.torch}'
export PATH=$PWD/.venv/bin:$PATH
py=.venv/bin/python
mkdir -p {run}/build
$py -c "import sys; sys.path.insert(0, 'src/scripts/navsim/gpumem'); import bench; print(bench.build_reference('src', '{run}/build/libnavmemref.so'))" > {run}/reference-build.txt
$py src/scripts/navsim/gpumem/bench.py metadata --output {run}/metadata.json
{parity}
$py src/scripts/navsim/gpumem/bench.py scale --build {run}/build --agents {agents} --thread-per-agent "{single}" \\
  --float-confidence "{fast}" \\
  --warmup {args.warmup} --repeats {args.repeats} --vram-limit-gib {args.vram_limit_gib} --output {run}/scale.json --captures {capture_list}
nvidia-smi --query-gpu=memory.used,memory.free --format=csv > {run}/vram-after.txt
df -h ~ | tail -1
"""
    job = [str(ROOT / "scripts/gq"), "--host", host, "run", "--class", "quick", "--threads", "4", "--mem", "8G",
           "--gpu-mem", str(args.gpu_mem), "--max", str(args.max_seconds), "--wait-max", "1800",
           "--name", "navsim-gpumem", "--", "bash", "-lc", body]
    code = subprocess.run(job).returncode
    print(f"gq exit {code}; raw results in {host}:{run}", flush=True)
    if code == 0:
        collect(host, run, stamp, args)
    sys.exit(code)


def collect(host, run, stamp, args):
    local = Path("/tmp") / f"navsim-gpumem-{stamp}"
    local.mkdir(parents=True, exist_ok=True)
    ssh(host, f"rm -rf {run}/build")
    subprocess.run(["scp", "-q", "-r", f"{host}:{run}/.", str(local)], check=True)
    digests = {p.name: sha256(p) for p in sorted(local.iterdir()) if p.is_file()}
    (local / "SHA256SUMS").write_text("".join(f"{d}  {n}\n" for n, d in digests.items()))
    nas = f"{NAS}/{stamp}"
    subprocess.run(["ssh", "ryzen", f"mkdir -p {nas}"], check=True)
    subprocess.run(["scp", "-q", *[str(p) for p in sorted(local.iterdir())], f"ryzen:{nas}/"], check=True)
    remote_sums = subprocess.check_output(["ssh", "ryzen", f"cd {nas} && sha256sum -c SHA256SUMS"], text=True)
    print(remote_sums, flush=True)
    summarise(local, nas, digests, args)


def summarise(local, nas, digests, args):
    metadata = json.loads((local / "metadata.json").read_text())
    used, free = (int(v) for v in re.findall(r"(\d+) MiB", (local / "vram-after.txt").read_text()))
    report = json.loads(REPORT.read_text()) if REPORT.exists() else {}
    report.update(status="measured", method=__doc__.strip(), implementation=IMPLEMENTATION,
                  warp="not used (CUDA C++ extension, see implementation)")
    run = dict(stamp=local.name.rsplit("-", 1)[-1], raw_dir=nas, agents=args.agents, gpu=metadata["gpu"],
               torch=metadata["torch"], torch_cuda=metadata["torch_cuda"], nvcc=metadata["nvcc"][-2],
               gcc=metadata["gcc"], glibc=metadata["glibc"], cuda_flags=metadata["cuda_flags"],
               free_vram_mib_before=metadata["free_vram_mib"], free_vram_mib_after=free, used_vram_mib_after=used,
               gq=dict(host="dev-ubuntu", cls="quick", threads=4, mem_gib=8, gpu_mem_gib=args.gpu_mem),
               raw_checksums=dict(path=f"{nas}/SHA256SUMS", sha256=sha256(local / "SHA256SUMS"), files=digests))
    for name, key in (("parity.json", "parity"), ("parity-float-confidence.json", "parity_float_confidence")):
        if (local / name).exists():
            parity = json.loads((local / name).read_text())
            report[key] = dict(variant=parity["variant"], records=parity["records"], synthetic=parity["synthetic"],
                               slope_tan=parity["slope_tan"], raw=f"{nas}/{name}", raw_sha256=digests[name])
    scale = json.loads((local / "scale.json").read_text())
    run["cells"] = [{k: v for k, v in c.items() if not k.endswith("_per_repeat")} for c in scale["cells"]]
    report.setdefault("runs", []).append(run)
    report["headline"] = headline(report)
    write_json(REPORT, report)
    print(f"wrote {REPORT}", flush=True)


def headline(report):
    """Latest numbers per N and variant, for reading without the cell detail."""
    cells = {}
    for run in report["runs"]:
        for c in run["cells"]:
            cells[(c["agents"], c["lanes"], c.get("variant", "exact"))] = (run["stamp"], c)
    rows = []
    for (n, lanes, variant), (stamp, c) in sorted(cells.items()):
        row = dict(agents=n, lanes=lanes, variant=variant, run=stamp, status=c.get("status"))
        if c.get("status") == "ok":
            row.update(update_us_per_agent=c["update_us_per_agent"]["median"],
                       readout_us_per_agent=c["readout_us_per_agent"]["median"],
                       memory_us_per_decision=c["memory_us_per_decision"],
                       agent_updates_per_second=c["agent_updates_per_second"]["median"],
                       process_vram_mib=c["process_vram_mib"], peak_allocated_mib=c["peak_allocated_bytes"] / 2 ** 20)
        else:
            row.update(free_vram_mib_before=c.get("free_vram_mib_before"), error=c.get("error"))
        rows.append(row)
    parity = report.get("parity", {}).get("records", {}).get("total", {})
    return dict(parity=dict(readouts=parity.get("readouts"), bitwise_equal_fraction=parity.get("bitwise_equal_fraction"),
                            differing_values=parity.get("differing_values"),
                            action_equal_fraction=parity.get("actions", {}).get("equal_fraction")),
                state_bytes_per_agent=report["runs"][-1]["cells"][0].get("bytes_per_agent"), scale=rows)


IMPLEMENTATION = dict(
    choice="CUDA C++ through a PyTorch extension (torch.utils.cpp_extension.load), not Warp",
    reasons=[
        "Bitwise parity needs explicit compiler control: nvcc --fmad=false, IEEE division/sqrt, no fast math; "
        "the same header also builds for the CPU (one lane) to check logic against the engine without a GPU.",
        "Order-preserving ingestion is warp-synchronous: one warp per agent and grid, rays in slot order with "
        "__syncwarp between rays, the 32 lanes splitting one ray's distinct DDA cells; this relies on "
        "__syncwarp/__ballot_sync, which Warp kernels do not expose.",
        "The readout writes torch CUDA tensors in place (CHW float32), the HD net's input batch.",
    ],
    kernels=dict(prepare="warp per agent: pose, reset, path history, fresh slots (ballot compaction), water list",
                 ingest="warp per agent and grid: centring, familiar window, ordered ray DDA, water patches",
                 read="thread per output cell: gathers the <=3x3 world cells mapping into it in world order, "
                      "classifies, merges by the engine's precedence, quantises, writes 10 planes"),
    source="scripts/navsim/gpumem/memory_kernels.h",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    agents = lambda value: [int(v) for v in value.split(",") if v]  # noqa: E731
    p = sub.add_parser("parity")
    p.add_argument("--backend", choices=("cuda", "host"), default="cuda")
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--reference", type=Path, required=True)
    p.add_argument("--weights", type=Path, required=True, help="policy.navbc for NavPolicy::encode")
    p.add_argument("--policy", type=Path, help="last.pt: compare HD net actions on CUDA")
    p.add_argument("--lanes", type=int, default=32)
    p.add_argument("--variant", choices=tuple(gpumem.VARIANTS), default="exact")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--captures", nargs="+", required=True)
    for name in ("scale", "cell"):
        p = sub.add_parser(name)
        p.add_argument("--build", type=Path, required=True)
        p.add_argument("--agents", type=agents if name == "scale" else int, required=True)
        p.add_argument("--warmup", type=int, default=3)
        p.add_argument("--repeats", type=int, default=20)
        p.add_argument("--vram-limit-gib", type=float, default=0)
        p.add_argument("--output", type=Path, required=True)
        p.add_argument("--captures", nargs="+", required=True)
        if name == "scale":
            p.add_argument("--thread-per-agent", type=agents, default=[])
            p.add_argument("--float-confidence", type=agents, default=[])
        else:
            p.add_argument("--lanes", type=int, default=32)
            p.add_argument("--variant", choices=tuple(gpumem.VARIANTS), default="exact")
    p = sub.add_parser("metadata")
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("remote")
    p.add_argument("--agents", type=agents, default=[64, 256, 1024, 2048])
    p.add_argument("--thread-per-agent", type=agents, default=[1024], help="one thread per agent, for contrast")
    p.add_argument("--float-confidence", type=agents, default=[1024, 2048],
                   help="also time the non-bitwise float-confidence variant at these N")
    p.add_argument("--warmup", type=int, default=3)
    p.add_argument("--repeats", type=int, default=20)
    p.add_argument("--gpu-mem", type=int, default=6, help="GiB declared to gq")
    p.add_argument("--vram-limit-gib", type=float, default=5.5, help="torch allocator cap inside the job")
    p.add_argument("--max-seconds", type=int, default=3600)
    p.add_argument("--torch", default="2.14.1", help="PyPI build (CUDA 13.0) in the uv cache")
    p.add_argument("--skip-parity", action="store_true")
    args = parser.parse_args()
    if getattr(args, "warmup", 3) < 3 or getattr(args, "repeats", 20) < 20:
        parser.error("at least 3 warmups and 20 repeats")
    dict(parity=parity_command, scale=scale_command, cell=cell_command, metadata=metadata_command,
         remote=remote_command)[args.command](args)


if __name__ == "__main__":
    main()
