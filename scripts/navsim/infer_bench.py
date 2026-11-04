#!/usr/bin/env python3
"""Measure the pace-v5 policy on Ryzen, without a simulator or a corpus.

One command from this checkout (uv setup, execution and download included)::

    python3 scripts/navsim/infer_bench.py --remote --gq-class quick

Use quiet when admission is expected within 30 minutes. The launcher submits
through scripts/gq with --threads 8 --mem 6G --gpu-mem 6G; uv creates
~/gothic-ai/nav-sim-research/inference/.venv. Raw samples go through gq --archive
to /mnt/shared/gothic-ai/nav-sim-research/inference/<run>/, and only results.json
is downloaded into the research data/inference directory. Remove .venv after
the final run. No model is executed on the launching Mac.

Method / preflight research:
* Reference: current training is B=8, T=64, about 1300 decisions/s (pace-v5
  report). NavPolicy's CNN consumes B*T maps before its GRU; resident fp32
  B=256,T=64 inputs alone need 6,103,171,072 bytes, before activations.
* CUDA graphs require static addresses: retain inputs and copy the returned
  hidden state into its fixed input buffer on every replay. Warm up outside
  capture; synchronize before and inside each timed interval. Disable TF32
  for the fp32 batch-versus-single acceptance check (absolute error <=1e-4).
  https://docs.pytorch.org/docs/2.9/notes/cuda.html#cuda-graphs
* Pre-pin source buffers outside transfer timing; pageable and pinned copies
  both use non_blocking=True plus a measured final synchronization. Report
  payload bandwidth, without allocation or unpacking in transfer-only cells.
  https://docs.pytorch.org/tutorials/intermediate/pinmem_nonblock.html
* nav_bc_data._pack stores 7 probability + 1 validity channels as uint8,
  two height channels as fp16, and the vector as fp16. "uint8" below means
  this actual mixed layout, not quantizing heights. Streaming training copies
  and unpacks each of 64 time steps and retains the GRU graph until backward.

Training is an explicitly simpler analogue: four action-head cross-entropies,
scaled forward=2, strafe=1, turn=2, maneuver=1, divided by 7; AdamW lr=3e-4,
weight_decay=1e-4, gradient clipping at 1, hidden detached only between segments.
No auxiliary heads, masks, class/family weights or maneuver focal term. Inputs
and balanced synthetic labels are pre-generated, with no disk IO in timings.
Failed capacity/capture cells are explicit, never replaced with smaller batches.
Each cell runs in a fresh process to release graph pools and isolate CUDA OOM.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
REPORT = Path("reports/navigation/2026-11-04-nav-sim-research/data/inference/results.json")
WEIGHTS = "/mnt/shared/gothic-ai/nav-pace-v5/run/last.pt"
BATCHES = (1, 64, 1024, 4096)
GIB = 1024 ** 3


def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def load_snapshot():
    return dict(utc=datetime.now(timezone.utc).isoformat(), load_average=os.getloadavg(),
                gq_status=command(str(Path.home() / "gothic-ai/bin/gq"), "status"),
                gpu=command("nvidia-smi", "--query-gpu=memory.used,utilization.gpu,utilization.memory",
                            "--format=csv,noheader"))


def launch_remote(args):
    task = command("ssh", "ryzen", "printf '%s/gothic-ai/nav-sim-research/inference' \"$HOME\"")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = f"{task}/{stamp}"
    archive = f"/mnt/shared/gothic-ai/nav-sim-research/inference/{stamp}"
    command("ssh", "ryzen", f"mkdir -p {shlex.quote(task + '/scripts/navsim')}")
    files = ["scripts/navsim/infer_bench.py", "scripts/nav_bc_model.py",
             "scripts/nav_bc_data.py", "scripts/nav_demos.py"]
    # Only the task's code is copied; no other worktree or remote task is touched.
    for name in files:
        subprocess.run(["scp", str(ROOT / name), f"ryzen:{task}/{name}"], check=True)
    body = f"""set -eu
df -h ~
test $(df --output=avail -B1 ~ | tail -1) -ge {20 * GIB}
cd {shlex.quote(task)}
if [ ! -x .venv/bin/python ]; then $HOME/.local/bin/uv venv --python 3.12 .venv; fi
$HOME/.local/bin/uv pip install --python .venv/bin/python numpy 'torch==2.9.1+cu128' --index-url https://download.pytorch.org/whl/cu128
exec .venv/bin/python scripts/navsim/infer_bench.py --output {shlex.quote(output + '/results.json')} --gq-class {args.gq_class} --threads {args.threads}
"""
    job = command(str(ROOT / "scripts/gq"), "submit", "--class", args.gq_class,
                  "--threads", str(args.threads), "--mem", "6G", "--gpu-mem", "6G",
                  "--max", "3600", "--name", "navsim-inference", "--archive", output,
                  "--", "bash", "-lc", body)
    print(f"gq submitted {job}; reconnect with scripts/gq wait {job}", flush=True)
    subprocess.run([str(ROOT / "scripts/gq"), "wait", job], check=True)
    (ROOT / REPORT).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["scp", f"ryzen:{archive}/results.json", str(ROOT / REPORT)], check=True)


def setup_torch(args, cell):
    # Bind before importing torch so OpenMP workers inherit this CPU set.
    cpus = sorted(os.sched_getaffinity(0))[:cell.get("threads", args.threads)]
    os.sched_setaffinity(0, cpus)
    os.environ["OMP_NUM_THREADS"] = str(len(cpus))
    os.environ["MKL_NUM_THREADS"] = str(len(cpus))
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    global torch, np, NavPolicy, HD_NEAR_SHAPE, HD_FAR_SHAPE, PACE_VECTOR_SIZE
    import numpy as np
    import torch
    from nav_bc_model import NavPolicy
    from nav_bc_data import HD_NEAR_SHAPE, HD_FAR_SHAPE, PACE_VECTOR_SIZE
    torch.set_num_threads(len(cpus))
    torch.set_num_interop_threads(1)
    torch.manual_seed(20261104)
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    torch.backends.cudnn.benchmark = False
    if cell.get("device") == "cuda":
        # Leave 768 MiB of the declared 6 GiB for CUDA context/non-allocator use.
        torch.cuda.set_per_process_memory_fraction(5.25 * GIB / torch.cuda.get_device_properties(0).total_memory)
    return cpus


def policy(args, device):
    state = torch.load(args.weights, map_location="cpu", weights_only=False)
    config = {**state["config"], "auxiliary": False}
    assert config["format_version"] == 5 and config["vector_size"] == PACE_VECTOR_SIZE
    model = NavPolicy(**config)
    model.load_state_dict({k: v for k, v in state["model"].items() if not k.startswith("aux_")})
    return model.to(device)


def inputs(batch, length=1, *, device="cpu", packed=False, pinned=False):
    result = {}
    for name, shape in (("near", HD_NEAR_SHAPE), ("far", HD_FAR_SHAPE)):
        if packed:
            for suffix, channels, dtype in (("channels", 7, torch.uint8),
                                             ("height", 2, torch.float16),
                                             ("valid", 1, torch.uint8)):
                value = torch.empty(batch, length, channels, *shape[1:], dtype=dtype,
                                    device=device, pin_memory=pinned)
                value.random_(0, 256) if dtype == torch.uint8 else value.uniform_(-1, 1)
                result[f"{name}_{suffix}"] = value
        else:
            result[name] = torch.empty(batch, length, *shape, device=device,
                                       pin_memory=pinned).uniform_(0, 1)
    result["vector"] = torch.empty(batch, length, PACE_VECTOR_SIZE, device=device,
                                   dtype=torch.float16 if packed else torch.float32,
                                   pin_memory=pinned).uniform_(-1, 1)
    return result


def unpack(values):
    return {**{name: torch.cat((values[f"{name}_channels"].float().div_(255),
                               values[f"{name}_height"].float(),
                               values[f"{name}_valid"].float().div_(255)), dim=2)
               for name in ("near", "far")}, "vector": values["vector"].float()}


def measure(operation, args, cell, payload=0):
    cuda = cell["device"] == "cuda"
    for _ in range(args.warmup):
        operation()
    if cuda:
        torch.cuda.synchronize()
        torch.cuda.reset_peak_memory_stats()
    samples = []
    for _ in range(args.repeats):
        if cuda:
            torch.cuda.synchronize()
        start = time.perf_counter_ns()
        operation()
        if cuda:
            torch.cuda.synchronize()
        samples.append((time.perf_counter_ns() - start) / 1e6)
    stats = lambda values: dict(median=float(np.median(values)), p90=float(np.percentile(values, 90)))
    result = dict(status="ok", repeats=len(samples), warmup=args.warmup, ms=stats(samples),
                  peak_vram_allocated_bytes=torch.cuda.max_memory_allocated() if cuda else 0,
                  peak_vram_reserved_bytes=torch.cuda.max_memory_reserved() if cuda else 0)
    if payload:
        result.update(payload_bytes=payload, GB_per_s=stats(payload / np.asarray(samples) / 1e6))
    else:
        decisions = cell["batch"] * (64 if cell["kind"] == "training" else 1)
        result["decisions_per_s"] = stats(decisions * 1000 / np.asarray(samples))
    write_json(args.output.with_suffix(".samples.json"), samples)
    return result


def inference(args, cell):
    model = policy(args, cell["device"]).eval()
    values = inputs(cell["batch"], device=cell["device"])
    hidden = torch.zeros(1, cell["batch"], 256, device=cell["device"])
    dtype = {"fp32": torch.float32, "fp16": torch.float16, "bf16": torch.bfloat16}[cell["precision"]]
    with torch.inference_mode(), torch.autocast("cuda", dtype=dtype, enabled=dtype != torch.float32):
        def step():
            _, new_hidden = model(**values, hidden=hidden)
            hidden.copy_(new_hidden)

        if cell["mode"] == "graph":
            stream = torch.cuda.Stream()
            stream.wait_stream(torch.cuda.current_stream())
            with torch.cuda.stream(stream):
                for _ in range(args.warmup):
                    step()
            torch.cuda.current_stream().wait_stream(stream)
            torch.cuda.synchronize()
            graph = torch.cuda.CUDAGraph()
            with torch.cuda.graph(graph):
                step()
            return measure(graph.replay, args, cell)
        return measure(step, args, cell)


def transfer(args, cell):
    source = inputs(cell["batch"], packed=cell["layout"] == "uint8", pinned=cell["pinned"])
    target = {k: torch.empty_like(v, device="cuda") for k, v in source.items()}

    def step():
        for name, value in source.items():
            target[name].copy_(value, non_blocking=True)

    return measure(step, args, cell, sum(v.numel() * v.element_size() for v in source.values()))


def training(args, cell):
    model = policy(args, "cuda").train()
    optimizer = torch.optim.AdamW(model.parameters(), lr=3e-4, weight_decay=1e-4)
    streaming = cell["source"] == "cpu_uint8_per_step"
    # Time-major CPU storage keeps each transferred time slice contiguous/pinned.
    values = inputs(64, cell["batch"], packed=True, pinned=True) if streaming else inputs(
        cell["batch"], 64, device="cuda")
    labels = {h: torch.randint(n, (cell["batch"], 64), device="cuda") for h, n in model.head_sizes.items()}
    hidden = torch.zeros(1, cell["batch"], 256, device="cuda")
    scales = dict(forward=2, strafe=1, turn=2, maneuver=1)

    def step():
        nonlocal hidden
        optimizer.zero_grad(set_to_none=True)
        if streaming:
            outputs = {h: [] for h in model.head_sizes}
            for t in range(64):
                transferred = {k: v[t:t + 1].transpose(0, 1).to("cuda", non_blocking=True)
                               for k, v in values.items()}
                logits, hidden = model(**unpack(transferred), hidden=hidden)
                for h, value in logits.items():
                    outputs[h].append(value)
            logits = {h: torch.cat(v, dim=1) for h, v in outputs.items()}
        else:
            logits, hidden = model(**values, hidden=hidden)
        hidden = hidden.detach()
        loss = sum(scales[h] * torch.nn.functional.cross_entropy(v.flatten(0, 1), labels[h].flatten())
                   for h, v in logits.items()) / 7
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.)
        optimizer.step()

    return measure(step, args, cell)


def correctness(args, cell):
    model = policy(args, cell["device"]).eval()
    values = inputs(4096, device=cell["device"])
    hidden = torch.randn(1, 4096, 256, device=cell["device"]) * .1
    errors = {h: 0. for h in model.head_sizes}
    hidden_error = 0.
    with torch.inference_mode():
        batched, batched_hidden = model(**values, hidden=hidden)
        for i in range(4096):
            single, single_hidden = model(**{k: v[i:i + 1] for k, v in values.items()}, hidden=hidden[:, i:i + 1])
            for head in errors:
                errors[head] = max(errors[head], float((batched[head][i:i + 1] - single[head]).abs().max()))
            hidden_error = max(hidden_error, float((batched_hidden[:, i:i + 1] - single_hidden).abs().max()))
    return dict(status="ok" if max(*errors.values(), hidden_error) <= 1e-4 else "failed",
                agents=4096, precision="fp32", nonzero_initial_hidden=True,
                max_logit_error=errors, max_hidden_error=hidden_error, tolerance=1e-4)


def metadata(args):
    checkpoint = torch.load(args.weights, map_location="cpu", weights_only=False)
    paths = [Path(args.weights), Path(args.weights).with_name("policy.navbc")]
    return dict(python=sys.version, torch=torch.__version__, cuda=torch.version.cuda,
                cudnn=torch.backends.cudnn.version(), torch_build=torch.__config__.show(),
                compiler=command("gcc", "--version").splitlines()[0],
                custom_compiler_flags="none; prebuilt torch wheel, see torch_build for -O/-march flags",
                cpu=command("lscpu"), gpu=command("nvidia-smi", "--query-gpu=name,driver_version,memory.total",
                                                "--format=csv,noheader"),
                checkpoint_config=checkpoint["config"],
                weights={str(p): hashlib.file_digest(p.open("rb"), "sha256").hexdigest() for p in paths},
                script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                tf32=False, cudnn_benchmark=False, gpu_budget_bytes=6 * GIB,
                torch_allocator_limit_bytes=int(5.25 * GIB), snapshot=load_snapshot())


def worker(args):
    cell = json.loads(args.cell)
    cpus = setup_torch(args, cell)
    result = dict(cell, cpu_affinity=cpus)
    try:
        result.update({"metadata": metadata(args)} if cell["kind"] == "metadata" else
                      globals()[cell["kind"]](args, cell))
    except torch.cuda.OutOfMemoryError as exc:
        result.update(status="out_of_memory", error=str(exc), repeats=0, warmup_completed=False,
                      ms=None, decisions_per_s=None)
    write_json(args.output, result)


def run(args):
    if not args.output.is_absolute():
        raise ValueError("--output must be absolute")
    if args.repeats < 30 or args.warmup < 5:
        raise ValueError("at least 30 repetitions and 5 warmups are required")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    cells = [dict(kind="metadata")]
    cells += [dict(kind="inference", device="cpu", batch=b, precision="fp32", mode="eager", threads=t)
              for b in BATCHES for t in (1, args.threads)]
    cells += [dict(kind="inference", device="cuda", batch=b, precision=p, mode=m)
              for b in BATCHES for p in ("fp32", "fp16", "bf16") for m in ("eager", "graph")]
    cells += [dict(kind="transfer", device="cuda", batch=b, layout=d, pinned=p)
              for b in BATCHES for d in ("fp32", "uint8") for p in (False, True)]
    cells += [dict(kind="training", device="cuda", batch=b, segment=64, precision="fp32", source=s)
              for b in (8, 64, 256) for s in ("gpu_resident", "cpu_uint8_per_step")]
    cells += [dict(kind="correctness", device=d, batch=4096, threads=1) for d in ("cpu", "cuda")]
    report = dict(schema_version=1, gq_class=args.gq_class, gq_threads=args.threads,
                  method=__doc__, cells=[], started_utc=datetime.now(timezone.utc).isoformat())
    for index, cell in enumerate(cells):
        print(f"[{index + 1}/{len(cells)}] {json.dumps(cell)}", flush=True)
        dest = args.output.parent / f"cell-{index:02d}.json"
        before = load_snapshot()
        with dest.with_suffix(".log").open("w") as log:
            subprocess.run([sys.executable, str(Path(__file__).resolve()), "--cell", json.dumps(cell),
                            "--weights", args.weights, "--output", str(dest), "--threads", str(args.threads),
                            "--repeats", str(args.repeats), "--warmup", str(args.warmup)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        result = json.loads(dest.read_text())
        result.update(machine_before=before, machine_after=load_snapshot())
        report["cells"].append(result)
        write_json(args.output, report)
        print(f"  {result.get('status', 'ok')} {result.get('ms', '')}", flush=True)
        if result.get("status") == "failed":
            raise AssertionError("batch-versus-single correctness failed")
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    write_json(args.output, report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote", action="store_true")
    parser.add_argument("--gq-class", choices=("quiet", "quick", "batch"), default="quiet")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--weights", default=WEIGHTS)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--repeats", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--cell", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.remote:
        launch_remote(args)
    elif args.cell:
        worker(args)
    else:
        run(args)


if __name__ == "__main__":
    main()
