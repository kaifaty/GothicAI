#!/usr/bin/env python3
"""GPU half of nav-sim: current-decision memory, passage and the v5 net (S3/S4b).

Phase A returns ticks, eye, rewards, episode flags and passage queries. The
GPU updates memory once and returns passage verdicts to phase B. finish()
can request another round against that same memory, or return the encoded
roads/vector and teacher labels. Only then does the GPU run the policy.
The two halves overlap CPU A/B futures with GPU stages on separate streams.

Modes: fp32 eager for parity, fp16 with separate memory and policy CUDA
Graphs for speed. Copies and passage rounds stay outside the graphs.

One command from the Mac stages everything on Ryzen and submits one gq job
without waiting. The job writes the compact report alongside raw data on NAS::

    python3 scripts/navsim/stand/loop.py remote

Commands inside the job:
  parity  24 drive captures, one agent each from its first decision, through
          the loop in fp32 and the engine (navsim_memory_ref + the same net,
          each with its own GRU state): actions on every readout, readout bits
          by hash; then fp16 + graph against fp32
  speed   per N, in its own process: GPU part per decision (halves one by one),
          decisions/s of the pipelined loop with the replay, and with an
          artificial CPU cost of 180 us in A and 20 us in B on 24 threads
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED

import numpy as np

HERE = Path(__file__).resolve().parent
GPUMEM = HERE.parent / "gpumem"
for path in (HERE, GPUMEM):
    sys.path.insert(0, str(path))
import bench  # noqa: E402
import captures  # noqa: E402
import contract  # noqa: E402
import cpu_replay  # noqa: E402
import gpumem  # noqa: E402

ROOT = HERE.parents[2]
REPORT = ROOT / "reports/navigation/2026-11-04-nav-sim-research/data/stand/passage.json"
NAS = "/mnt/shared/gothic-ai/nav-sim-research/stand/passage"
REMOTE_TASK = "gothic-ai/nav-sim-stand-passage"
HISTORY = 1024
STEPS_PER_REPEAT = 30
DELAY_US, DELAY_THREADS = 200.0, 24
STAGED = (bench.STAGED + ["scripts/nas_archive.py"]
          + ["scripts/navsim/stand/" + n for n in ("loop.py", "contract.py", "cpu_replay.py")])
SPIN_C = r"""
#include <time.h>
void navsim_spin(double seconds) {
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  const double end = t.tv_sec + t.tv_nsec * 1e-9 + seconds;
  do clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); while (t.tv_sec + t.tv_nsec * 1e-9 < end);
}
"""


def torch_dtype(torch, dtype):
    # uint32 travels as int32: only copies touch it on the GPU.
    return {np.dtype(np.float32): torch.float32, np.dtype(np.uint32): torch.int32,
            np.dtype(np.uint8): torch.uint8, np.dtype(np.int32): torch.int32}[np.dtype(dtype)]


def load_policy(torch, weights):
    sys.path.insert(0, str(HERE.parents[1]))
    from nav_bc_model import NavPolicy
    state = torch.load(weights, map_location="cpu", weights_only=False)
    config = {**state["config"], "auxiliary": False}
    if config.get("vector_size") != contract.VECTOR:
        raise ValueError(f"{weights}: vector {config.get('vector_size')} != {contract.VECTOR}")
    model = NavPolicy(**config)
    model.load_state_dict({k: v for k, v in state["model"].items() if not k.startswith("aux_")})
    model = model.to("cuda").eval()
    if len(model.head_sizes) != contract.HEADS:
        raise ValueError("the contract carries four action heads")
    return model


def policy_step(torch, model, near, far, vector, hidden, first, half=False, sample=False):
    """Clear the GRU state of agents that start an episode, step the net, pick actions."""
    hidden.masked_fill_(first.view(1, -1, 1), 0)
    with torch.no_grad(), torch.autocast("cuda", dtype=torch.float16, enabled=half, cache_enabled=False):
        logits, new = model(near[:, None], far[:, None], vector[:, None], hidden=hidden)
    with torch.no_grad():
        hidden.copy_(new)
        picks = []
        for head in model.head_sizes:
            value = logits[head][:, 0].float()
            if sample:  # Gumbel-max: argmax(logits + Gumbel) samples the softmax
                value = value - torch.empty_like(value).exponential_().log()
            picks.append(value.argmax(-1))
        return torch.stack(picks, 1).to(torch.uint8)


class Half:
    """One half of the agents: its CPU side, memory, stream and net state."""

    def __init__(self, torch, backend, model, side, ids, *, journey=0, maps=(), map_ids=None,
                 half=False, graph=False, sample=False):
        self.torch, self.model, self.side, self.ids = torch, model, side, np.asarray(ids)
        self.half, self.sample = half, sample
        n = self.n = side.agents
        self.memory = gpumem.Memory(backend, n, contract.SLOTS, HISTORY, journey, maps, map_ids)
        self.fields, nbytes = contract.layout(n)
        self.staging = torch.zeros(nbytes, dtype=torch.uint8, device="cuda")
        self.view = {f.name: self.bytes(f).view(torch_dtype(torch, f.dtype)).view(f.shape)
                     for f in self.fields.values()}
        T = contract.TICKS
        self.tick_pos = torch.zeros(T, n, 3, device="cuda")
        self.tick_fwd = torch.zeros(T, n, 3, device="cuda")
        self.tick_limits = torch.zeros(T, n, 3, device="cuda")
        self.tick_ms = torch.zeros(T, n, dtype=torch.int32, device="cuda")
        self.tick_active = torch.zeros(T, n, dtype=torch.uint8, device="cuda")
        self.contexts = [self.memory.context(posePos=self.tick_pos[t].data_ptr(), poseFwd=self.tick_fwd[t].data_ptr(),
                                             poseLimits=self.tick_limits[t].data_ptr(),
                                             poseMs=self.tick_ms[t].data_ptr(), active=self.tick_active[t].data_ptr())
                         for t in range(T)]
        self.need = torch.arange(T, 0, -1, dtype=torch.uint8, device="cuda").view(T, 1)  # tick t needs ticks >= 3-t
        self.body_ms = self.memory.buffers["bodyMs"][0].view(torch.int32)
        self.readers = torch.arange(n, dtype=torch.int32, device="cuda")
        self.near = torch.zeros(n, 10, 64, 64, device="cuda")
        self.far = torch.zeros(n, 10, 50, 100, device="cuda")
        finish_fields, finish_bytes = contract.layout(n, contract.FINISH_FIELDS)
        self.finish_staging = torch.zeros(finish_bytes, dtype=torch.uint8, device="cuda")
        f = finish_fields["vector"]
        self.vector = self.finish_staging[f.offset:f.offset + f.nbytes].view(torch.float32).view(n, contract.VECTOR)
        query_fields, query_bytes = contract.layout(34 * n, (("agents", np.int32, ()),
                                                           ("segments", np.float32, (8,))))
        self.query_staging = torch.zeros(query_bytes, dtype=torch.uint8, device="cuda")
        self.query_view = {f.name: self.query_staging[f.offset:f.offset + f.nbytes]
                          .view(torch_dtype(torch, f.dtype)).view(f.shape) for f in query_fields.values()}
        self.verdicts = torch.zeros(34 * n, dtype=torch.uint8, device="cuda")
        self.verdicts_host = torch.zeros(34 * n, dtype=torch.uint8, pin_memory=True)
        self.verdicts_np = self.verdicts_host.numpy()
        self.hidden = torch.zeros(1, n, model.hidden_size, device="cuda")
        self.actions = torch.zeros(n, contract.HEADS, dtype=torch.uint8, device="cuda")
        self.actions_host = torch.zeros(n, contract.HEADS, dtype=torch.uint8, pin_memory=True)
        self.actions_np = self.actions_host.numpy()
        self.stream = torch.cuda.Stream()
        self.done = torch.cuda.Event()
        self.computed = torch.cuda.Event()
        self.spans = []
        self.timed = False
        # Buffers were zeroed and uploaded on the current stream; the half's stream must see them.
        self.stream.wait_stream(torch.cuda.current_stream())
        self.graph = None
        self.policy_graph = None
        if graph:
            self.capture()

    def bytes(self, field):
        return self.staging[field.offset: field.offset + field.nbytes]

    def memory_part(self):
        v = self.view
        first = v["first"].bool()
        # NavigationMemory::reset() at an episode start: the kernels reset when time goes back.
        self.body_ms.masked_fill_(first, -1)
        self.tick_pos.copy_(v["pose_pos"].transpose(0, 1))
        self.tick_fwd.copy_(v["pose_fwd"].transpose(0, 1))
        self.tick_limits.copy_(v["pose_limits"].transpose(0, 1))
        self.tick_ms.copy_(v["pose_ms"].transpose(0, 1))
        self.tick_active.copy_(v["ticks"].view(1, -1) >= self.need)
        last = contract.TICKS - 1
        for t in range(last):
            self.memory.update(self.contexts[t])
        for name, slot in contract.RAY_SLOTS.items():  # the eye refreshes at the last tick
            self.memory.buffers[slot][0].copy_(self.bytes(self.fields[name]))
        self.memory.update(self.contexts[last])
        self.memory.read(self.readers, self.near, self.far)
        return first

    def policy_part(self):
        self.actions.copy_(policy_step(self.torch, self.model, self.near, self.far, self.vector, self.hidden,
                                       self.view["first"].bool(), self.half, self.sample))

    def capture(self):
        torch = self.torch
        warm = torch.cuda.Stream()
        warm.wait_stream(torch.cuda.current_stream())
        with torch.cuda.stream(warm):
            for _ in range(3):
                self.memory_part()
                self.policy_part()
        torch.cuda.current_stream().wait_stream(warm)
        torch.cuda.synchronize()
        self.graph, self.policy_graph = torch.cuda.CUDAGraph(), torch.cuda.CUDAGraph()
        with torch.cuda.graph(self.graph, stream=self.stream):
            self.memory_part()
        with torch.cuda.graph(self.policy_graph, stream=self.stream):
            self.policy_part()
        torch.cuda.synchronize()
        self.hidden.zero_()

    def measured(self, name, function):
        if not self.timed:
            function()
            return
        start, end = (self.torch.cuda.Event(enable_timing=True) for _ in range(2))
        start.record()
        function()
        end.record()
        self.spans.append((name, start, end))

    def launch(self, batch, timed=False, after=None):
        """Phase A: copy, update/read memory once, then check this decision's queries."""
        self.timed, self.spans = timed, []
        with self.torch.cuda.stream(self.stream):
            self.measured("copy_in", lambda: self.staging.copy_(batch.tensor, non_blocking=True))
            if after is not None:
                self.stream.wait_event(after)
            self.measured("memory", self.graph.replay if self.graph is not None else self.memory_part)
        self.launch_queries(batch.queries)

    def launch_queries(self, queries, after=None):
        """Another round against unchanged memory; never repeats ticks or resets."""
        self.query_count = queries.count
        with self.torch.cuda.stream(self.stream):
            def upload():
                for name in ("agents", "segments"):
                    field = queries.fields[name]
                    row_bytes = field.nbytes // queries.n
                    source = queries.tensor[field.offset:field.offset + queries.count * row_bytes]
                    target = self.query_view[name][:queries.count].view(self.torch.uint8).reshape(-1)
                    target.copy_(source, non_blocking=True)
            self.measured("copy_in", upload)
            if after is not None:
                self.stream.wait_event(after)
            q = queries.count
            self.measured("passage", lambda: self.memory.passage(self.query_view["agents"][:q],
                                                                self.query_view["segments"][:q], self.verdicts[:q]))
            self.computed.record()
            self.measured("copy_out", lambda: self.verdicts_host[:q].copy_(self.verdicts[:q], non_blocking=True))
            self.done.record()

    def launch_policy(self, batch, after=None):
        with self.torch.cuda.stream(self.stream):
            self.measured("copy_in", lambda: self.finish_staging.copy_(batch.tensor, non_blocking=True))
            if after is not None:
                self.stream.wait_event(after)
            self.measured("net", self.policy_graph.replay if self.policy_graph is not None else self.policy_part)
            self.computed.record()
            self.measured("copy_out", lambda: self.actions_host.copy_(self.actions, non_blocking=True))
            self.done.record()

    def timing(self):
        result = dict(copy_in=0., memory=0., passage=0., net=0., copy_out=0.)
        for name, start, end in self.spans:
            result[name] += start.elapsed_time(end)
        result["compute"] = result["memory"] + result["passage"] + result["net"]
        result["total"] = result["copy_in"] + result["compute"] + result["copy_out"]
        return result


class Loop:
    """A/B CPU futures and GPU stages of two halves overlap independently.

    A half never has two CPU calls in flight. GPU stages wait for the previous
    half's compute event, while host work remains free to advance the other.
    """

    def __init__(self, halves):
        self.halves = halves
        self.agents = sum(h.n for h in halves)
        self.pool = ThreadPoolExecutor(len(halves))
        self.tail = None
        self.state = ["idle"] * len(halves)
        self.future = [None] * len(halves)
        self.rounds = [0] * len(halves)
        self.completed = [0] * len(halves)
        self.query_rounds = 0
        self.query_count = 0
        self.max_rounds = 0

    def start(self):
        self.drain()
        for i, h in enumerate(self.halves):
            self.future[i] = self.pool.submit(h.side.reset)
            self.state[i] = "cpu_a"
        self._pump(self.completed.copy())

    def _pump(self, targets, on_actions=None):
        while True:
            progress = False
            for i, h in enumerate(self.halves):
                state = self.state[i]
                if state.startswith("cpu_") and self.future[i].done():
                    result = self.future[i].result()
                    if state == "cpu_a":
                        self.rounds[i] = 0
                        h.launch(result, after=self.tail)
                        self.state[i] = "gpu_passage"
                    elif isinstance(result, contract.PassageQueries):
                        if self.rounds[i] >= 4:
                            raise RuntimeError("CPU finish exceeded four passage rounds")
                        h.launch_queries(result, after=self.tail)
                        self.state[i] = "gpu_passage"
                    else:
                        h.launch_policy(result, after=self.tail)
                        self.state[i] = "gpu_actions"
                    self.tail = h.computed
                    progress = True
                elif state == "gpu_passage" and h.done.query():
                    self.rounds[i] += 1
                    self.max_rounds = max(self.max_rounds, self.rounds[i])
                    self.query_rounds += 1
                    self.query_count += h.query_count
                    self.future[i] = self.pool.submit(h.side.finish, h.verdicts_np[:h.query_count])
                    self.state[i] = "cpu_b"
                    progress = True
                elif state == "gpu_actions" and self.completed[i] < targets[i] and h.done.query():
                    if on_actions is not None:
                        on_actions(h)
                    self.completed[i] += 1
                    self.future[i] = self.pool.submit(h.side.step, h.actions_np)
                    self.state[i] = "cpu_a"
                    progress = True
            if all(s == "gpu_actions" and c == t for s, c, t in zip(self.state, self.completed, targets)):
                return
            if not progress:
                gpu = [h for h, s in zip(self.halves, self.state)
                       if s == "gpu_passage" or (s == "gpu_actions" and self.completed[self.halves.index(h)]
                                                < targets[self.halves.index(h)])]
                pending = [f for f, s in zip(self.future, self.state) if s.startswith("cpu_")]
                if gpu and pending:
                    # A finished CPU half may upload its inputs while the
                    # other half still computes; do not block on that GPU.
                    wait(pending, timeout=.0002, return_when=FIRST_COMPLETED)
                elif gpu:
                    gpu[0].done.synchronize()
                else:
                    wait(pending, return_when=FIRST_COMPLETED)

    def advance(self, steps, on_actions=None):
        self._pump([c + steps for c in self.completed], on_actions)

    def drain(self, on_actions=None):
        for i, h in enumerate(self.halves):
            if self.state[i] == "gpu_actions":
                h.done.synchronize()
                if on_actions is not None:
                    on_actions(h)
                self.state[i] = "idle"

    def serial(self, steps):
        """No competing GPU work; report GPU spans excluding the CPU round trip."""
        self.drain()
        samples = []
        for _ in range(steps):
            for h in self.halves:
                h.launch(h.side.step(h.actions_np), timed=True)
                for round_index in range(4):
                    h.done.synchronize()
                    result = h.side.finish(h.verdicts_np[:h.query_count])
                    if not isinstance(result, contract.PassageQueries):
                        break
                    h.launch_queries(result)
                else:
                    raise RuntimeError("CPU finish exceeded four passage rounds")
                h.launch_policy(result)
                h.done.synchronize()
                samples.append(h.timing())
        return samples

    def close(self):
        self.drain()
        self.pool.shutdown()


class DelaySide:
    """A CpuSide plus ``us`` of thread CPU time per agent and step, spread over a pool."""

    def __init__(self, inner, spin, pool, threads, us=DELAY_US):
        self.inner, self.spin, self.pool, self.threads = inner, spin, pool, threads
        self.agents, self.us = inner.agents, us
        self.seconds = 0.0

    def reset(self):
        return self.inner.reset()

    def delay(self, us):
        started = time.perf_counter()
        each = self.agents * us * 1e-6 / self.threads
        list(self.pool.map(self.spin, [each] * self.threads))
        self.seconds += time.perf_counter() - started

    def step(self, actions):
        batch = self.inner.step(actions)
        self.delay(self.us * .9)
        return batch

    def finish(self, verdicts):
        result = self.inner.finish(verdicts)
        if not isinstance(result, contract.PassageQueries):
            self.delay(self.us * .1)
        return result


def spin_function(build):
    source, library = Path(build) / "spin.c", Path(build) / "libnavsimspin.so"
    source.write_text(SPIN_C)
    subprocess.run(["cc", "-O2", "-shared", "-fPIC", str(source), "-o", str(library)], check=True)
    lib = ctypes.CDLL(str(library))  # CDLL releases the GIL during the call
    lib.navsim_spin.argtypes = [ctypes.c_double]
    return lib.navsim_spin


def split(n):
    half = n // 2
    return [np.arange(0, half), np.arange(half, n)]


def agent_maps(records, ids):
    """Familiar atlases of the agents' captures (none in the drive records)."""
    maps, index, out = [], {}, []
    for a in ids:
        c = records[int(a) % len(records)]
        if not len(c.tiles):
            out.append(-1)
            continue
        if c.path not in index:
            index[c.path] = len(maps)
            maps.append((c.tiles, c.mask))
        out.append(index[c.path])
    return maps, np.array(out, np.int32)


def make_loop(torch, backend, model, replay, records, n, *, shift, half, graph, workers=1, journey=None,
              sample=False):
    halves = []
    for ids in split(n):
        side = cpu_replay.ReplaySide(replay, ids, shift=shift, pinned=True, workers=workers)
        maps, map_ids = agent_maps(records, ids)
        j = 0 if journey is None else np.asarray(journey, np.uint8)[ids]
        halves.append(Half(torch, backend, model, side, ids, journey=j, maps=maps, map_ids=map_ids,
                           half=half, graph=graph, sample=sample))
    return Loop(halves)


def stats(values):
    values = np.asarray(values, np.float64)
    return dict(median=float(np.median(values)), p90=float(np.percentile(values, 90)),
                min=float(values.min()), repeats=int(values.size))


def digest(array):
    return hashlib.blake2b(np.ascontiguousarray(array).view(np.uint8), digest_size=16).digest()


# ---- parity ----
def deterministic(torch):
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    torch.backends.cudnn.benchmark = False
    torch.backends.cudnn.deterministic = True


def parity_command(args):
    import torch
    deterministic(torch)
    records = [captures.read(p) for p in args.captures]
    N = len(records)
    replay = cpu_replay.Replay(records)
    steps = int(replay.count.max())
    model = load_policy(torch, args.policy)
    backend = gpumem.Backend("cuda", args.build)
    refs = [bench.Reference(args.reference, c.path, args.weights) for c in records]
    journey = [r.journey for r in refs]
    first_pass = np.arange(steps)[:, None] < replay.count[np.arange(N)][None, :]  # [steps, N]

    # Engine: navsim_memory_ref gives the maps and the vector; the same net, same half batches.
    engine = np.zeros((steps, N, contract.HEADS), np.uint8)
    hashes = {}
    vectors = np.zeros((replay.decisions, contract.VECTOR), np.float32)
    started = time.perf_counter()
    sides = [cpu_replay.ReplaySide(replay, ids, shift=False) for ids in split(N)]
    hidden = [torch.zeros(1, s.agents, model.hidden_size, device="cuda") for s in sides]
    for s in range(steps):
        for side, ids, h in zip(sides, split(N), hidden):
            batch = side.reset() if s == 0 else side.step(None)
            side.finish(np.zeros(batch.queries.count, np.uint8))
            near = np.empty((side.agents, 10, 64, 64), np.float32)
            far = np.empty((side.agents, 10, 50, 100), np.float32)
            vector = np.empty((side.agents, contract.VECTOR), np.float32)
            for k, a in enumerate(ids):
                if batch.first[k] and s > 0:  # the replay wrapped: a fresh engine memory
                    refs[a].close()
                    refs[a] = bench.Reference(args.reference, records[a].path, args.weights)
                if not refs[a].next():
                    raise RuntimeError(f"{records[a].path}: engine has fewer readouts")
                near[k], far[k], vector[k] = refs[a].near, refs[a].far, refs[a].vector
                if first_pass[s, a]:
                    vectors[side.decision[k]] = refs[a].vector
                    hashes[(s, int(a))] = (digest(near[k]), digest(far[k]))
            inputs = [torch.from_numpy(x).to("cuda") for x in (near, far, vector)]
            first = torch.from_numpy(batch.first.astype(bool)).to("cuda")
            engine[s, ids] = policy_step(torch, model, *inputs, h, first).cpu().numpy()
    for r in refs:
        r.close()
    engine_seconds = time.perf_counter() - started
    replay = cpu_replay.Replay(records, vectors=vectors)

    def run(half, graph):
        loop = make_loop(torch, backend, model, replay, records, N, shift=False, half=half, graph=graph,
                         journey=journey)
        actions = np.zeros((steps, N, contract.HEADS), np.uint8)
        state = dict(step=[0, 0], equal_maps=0, compared_maps=0, first=None, first_pass_queries=0)

        def collect(h):
            index = loop.halves.index(h)
            s = state["step"][index]
            state["step"][index] += 1
            actions[s, h.ids] = h.actions_np
            queries = h.side.batch.queries
            counts = np.bincount(queries.agents[:queries.count], minlength=h.n)
            state["first_pass_queries"] += int(counts[first_pass[s, h.ids]].sum())
            if half or graph:
                return
            near, far = h.near.cpu().numpy(), h.far.cpu().numpy()
            for k, a in enumerate(h.ids):
                if (s, int(a)) in hashes:
                    same = hashes[(s, int(a))] == (digest(near[k]), digest(far[k]))
                    state["compared_maps"] += 1
                    state["equal_maps"] += same
                    if not same and state["first"] is None:
                        state["first"] = dict(step=s, agent=int(a))

        began = time.perf_counter()
        loop.start()
        loop.advance(steps - 1, collect)
        loop.drain(collect)
        errors = np.bitwise_or.reduce(np.concatenate([h.memory.get("errors") for h in loop.halves]))
        result = dict(seconds=time.perf_counter() - began, state_errors=int(errors))
        result["passage"] = dict(queries=loop.query_count, half_rounds=loop.query_rounds,
                                 first_pass_queries=state["first_pass_queries"],
                                 max_rounds_observed=loop.max_rounds, round_limit=4)
        if not (half or graph):
            result.update(readout_maps=dict(compared=state["compared_maps"], bitwise_equal=state["equal_maps"],
                                            first_mismatch=state["first"]))
        loop.close()
        del loop
        torch.cuda.empty_cache()
        return actions, result

    def agreement(a, b, mask):
        same = (a == b).all(-1)
        heads = (a == b)[mask].mean(0)
        return dict(readouts=int(mask.sum()), equal=int(same[mask].sum()),
                    equal_fraction=float(same[mask].mean()), head_equal_fraction=[float(v) for v in heads])

    fp32, fp32_info = run(False, False)
    fp16, fp16_info = run(True, True)
    everything = np.ones_like(first_pass)
    report = dict(
        captures=[dict(path=c.path, decisions=int(d)) for c, d in zip(records, replay.count)],
        agents=N, steps=steps, journey=journey, heads=list(model.head_sizes), engine_seconds=engine_seconds,
        fp32=dict(**fp32_info, vs_engine_first_pass=agreement(fp32, engine, first_pass),
                  vs_engine_all_steps=agreement(fp32, engine, everything)),
        fp16_graph=dict(**fp16_info, vs_fp32_first_pass=agreement(fp16, fp32, first_pass),
                        vs_fp32_all_steps=agreement(fp16, fp32, everything),
                        vs_engine_first_pass=agreement(fp16, engine, first_pass)))
    mismatch = np.argwhere(~(fp32 == engine).all(-1) & first_pass)
    if len(mismatch):
        s, a = (int(v) for v in mismatch[0])
        report["fp32"]["first_action_mismatch"] = dict(step=s, agent=a, engine=engine[s, a].tolist(),
                                                       loop=fp32[s, a].tolist())
    bench.write_json(args.output, report)
    print(json.dumps({k: report[k] for k in ("fp32", "fp16_graph")}), flush=True)
    maps = report["fp32"]["readout_maps"]
    broken = (report["fp32"]["vs_engine_first_pass"]["equal_fraction"] != 1.0
              or maps["bitwise_equal"] != maps["compared"]
              or report["fp32"]["state_errors"] or report["fp16_graph"]["state_errors"])
    if broken:
        sys.exit("parity failed: fp32 actions or readouts differ from the engine, or memory state errors")


# ---- speed ----
def cell_command(args):
    import torch
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    total_vram = torch.cuda.get_device_properties(0).total_memory
    if args.vram_limit_gib:
        torch.cuda.set_per_process_memory_fraction(min(1.0, args.vram_limit_gib * 2 ** 30 / total_vram))
    N = args.agents
    result = dict(agents=N, precision=args.precision, graph=args.precision == "fp16", steps_per_repeat=STEPS_PER_REPEAT,
                  warmup=args.warmup, repeats=args.repeats, fill_workers=args.fill_workers,
                  free_vram_mib_before=int(bench.nvidia("memory.free")[0]), vram_limit_gib=args.vram_limit_gib,
                  status="ok")
    try:
        records = [captures.read(p) for p in args.captures]
        replay = cpu_replay.Replay(records)
        model = load_policy(torch, args.policy)
        backend = gpumem.Backend("cuda", args.build)
        torch.cuda.reset_peak_memory_stats()
        half = args.precision == "fp16"
        loop = make_loop(torch, backend, model, replay, records, N, shift=True, half=half, graph=half,
                         workers=args.fill_workers)
        result["setup_peak_allocated_bytes"] = torch.cuda.max_memory_allocated()
        loop.start()
        total = args.warmup + args.repeats

        # 1. GPU part: halves one by one.
        gpu_us, parts = [], []
        for repeat in range(total):
            samples = loop.serial(STEPS_PER_REPEAT)
            if repeat < args.warmup:
                continue
            gpu_us.append(sum(s["total"] for s in samples) * 1000 / (STEPS_PER_REPEAT * N))
            parts.append({k: sum(s[k] for s in samples) * 1000 / (STEPS_PER_REPEAT * N) for k in samples[0]})
        result["gpu_us_per_decision"] = stats(gpu_us)
        result["gpu_parts_us_per_decision"] = {k: float(np.median([p[k] for p in parts])) for k in parts[0]}
        result["gpu_us_per_decision_per_repeat"] = gpu_us

        def pipelined(label):
            loop.start()
            for h in loop.halves:
                h.side.seconds = 0.0
            rates, stamp = [], None
            for repeat in range(total):
                if repeat == args.warmup:
                    stamp = time.perf_counter()
                    loop.query_rounds = loop.query_count = loop.max_rounds = 0
                    for h in loop.halves:
                        h.side.seconds = 0.0
                loop.advance(STEPS_PER_REPEAT)
                if repeat >= args.warmup:
                    now = time.perf_counter()
                    rates.append(STEPS_PER_REPEAT * N / (now - stamp))
                    stamp = now
            loop.drain()
            cpu = sum(h.side.seconds for h in loop.halves)
            result[label] = dict(decisions_per_second=stats(rates), per_repeat=rates,
                                 cpu_side_ms_per_half_step=cpu * 1000 / (2 * args.repeats * STEPS_PER_REPEAT),
                                 passage=dict(queries=loop.query_count, half_rounds=loop.query_rounds,
                                              max_rounds_observed=loop.max_rounds,
                                              queries_per_decision=loop.query_count / (args.repeats * STEPS_PER_REPEAT * N)))

        # 2. The pipelined loop with the replay as the CPU side.
        pipelined("loop_replay")
        result["replay_fill_ms_per_half_step"] = result["loop_replay"]["cpu_side_ms_per_half_step"]
        # 3. The same plus 200 us of CPU thread time per decision on 24 threads.
        spin = spin_function(args.build)
        pool = ThreadPoolExecutor(DELAY_THREADS)
        replay_sides = [h.side for h in loop.halves]
        for h in loop.halves:
            h.side = DelaySide(h.side, spin, pool, DELAY_THREADS)
        pipelined("loop_delay")
        # Its cpu_side_ms_per_half_step is the spin alone; the replay's fill comes on top.
        result["loop_delay"].update(us_per_decision=DELAY_US, phase_a_us=180., phase_b_us=20., threads=DELAY_THREADS)
        result["passage"] = dict(queries=loop.query_count, half_rounds=loop.query_rounds, max_rounds=4,
                                 rebuild_fraction=.02, rebuild_queries=34)
        pool.shutdown()
        for h, side in zip(loop.halves, replay_sides):
            h.side = side
            side.close()
        loop.close()
        torch.cuda.synchronize()
        errors = np.bitwise_or.reduce(np.concatenate([h.memory.get("errors") for h in loop.halves]))
        result.update(peak_allocated_bytes=torch.cuda.max_memory_allocated(),
                      peak_reserved_bytes=torch.cuda.max_memory_reserved(), process_vram_mib=bench.process_vram_mib(),
                      free_vram_mib_after=int(bench.nvidia("memory.free")[0]), state_errors=int(errors),
                      step_batch_bytes_per_agent=contract.layout(1024)[1] / 1024,
                      replay_table_bytes=replay.nbytes())
    except torch.cuda.OutOfMemoryError as error:
        result.update(status="out_of_memory", error=str(error).splitlines()[0],
                      peak_allocated_bytes=torch.cuda.max_memory_allocated())
    bench.write_json(args.output, result)


def speed_command(args):
    cells = []
    for precision in args.precisions:
        for n in args.agents if precision == "fp16" else args.agents[:1]:
            output = Path(args.output).with_name(f"cell-{precision}-n{n}.json")
            output.unlink(missing_ok=True)
            command = [sys.executable, str(Path(__file__).resolve()), "cell", "--agents", str(n), "--precision",
                       precision, "--build", str(args.build), "--policy", str(args.policy), "--output", str(output),
                       "--warmup", str(args.warmup), "--repeats", str(args.repeats), "--fill-workers",
                       str(args.fill_workers), "--vram-limit-gib", str(args.vram_limit_gib), "--captures",
                       *args.captures]
            print(f"[speed] {precision} N={n}", flush=True)
            run = subprocess.run(command)
            cell = (json.loads(output.read_text()) if run.returncode == 0 and output.exists()
                    else dict(agents=n, precision=precision, status="crashed", exit_code=run.returncode))
            cells.append(cell)
            print("  " + json.dumps({k: cell.get(k) for k in ("status", "gpu_us_per_decision")}), flush=True)
            for label in ("loop_replay", "loop_delay"):
                if label in cell:
                    print(f"  {label} {cell[label]['decisions_per_second']}", flush=True)
    bench.write_json(args.output, dict(cells=cells))
    if any(c.get("status") != "ok" or c.get("state_errors") for c in cells):
        raise SystemExit("speed failed: a cell crashed, exceeded VRAM or reported memory errors")


# ---- launcher (Mac) ----
def remote_command(args):
    memory = json.loads(bench.MEMORY_REPORT.read_text())
    traces = memory["traces"]
    host = args.host
    home = bench.ssh(host, 'printf %s "$HOME"').stdout
    task = f"{home}/{REMOTE_TASK}"
    stamp = bench.run_stamp()
    run = f"{task}/runs/{stamp}"
    src = f"{run}/src"
    raw = f"{run}/raw"
    print(bench.ssh(host, f"df -h ~ | tail -1; mkdir -p {src} {task}/captures {task}/weights").stdout, flush=True)
    archive = Path("/tmp") / f"navsim-stand-src-{stamp}.tar"
    subprocess.run(["tar", "-cf", str(archive), "-C", str(ROOT), *STAGED, "OpenGothic/lib/rapidjson/include"],
                   check=True)
    subprocess.run(["scp", "-q", str(archive), f"{host}:{run}/src.tar"], check=True)
    archive.unlink()
    bench.ssh(host, f"tar -xf {run}/src.tar -C {src} && rm {run}/src.tar")
    wanted = [(t["path"], f"{task}/captures/{Path(t['path']).parents[1].name}.bin", t["sha256"]) for t in traces]
    weights = [(f"{bench.WEIGHTS}/policy.navbc", f"{task}/weights/policy.navbc", memory["weights"]["v5"]["sha256"]),
               (f"{bench.WEIGHTS}/last.pt", f"{task}/weights/last.pt", None)]
    bench.stage(host, wanted + weights)
    capture_list = " ".join(shlex.quote(t) for _, t, _ in wanted)
    agents = ",".join(str(n) for n in args.agents)
    script = f"{src}/scripts/navsim/stand/loop.py"
    parity = "" if args.skip_parity else f"""
$py {src}/scripts/navsim/gpumem/passage.py --backend cuda --build {run}/build \\
  --reference {run}/build/libnavmemref.so --weights {task}/weights/policy.navbc \\
  --output {raw}/passage-parity.json --captures {capture_list}
$py {script} parity --build {run}/build --reference {run}/build/libnavmemref.so \\
  --weights {task}/weights/policy.navbc --policy {task}/weights/last.pt --output {raw}/parity.json --captures {capture_list}
"""
    destination = f"nav-sim-research/stand/passage/{stamp}"
    body = f"""set -eu
cd {task}
finish() {{
  result=$?
  trap - EXIT
  python3 {src}/scripts/nas_archive.py move {raw} \\
    --destination-relative {destination} --attempt-id {stamp} || result=1
  rm -rf {run}/build {run}/src
  exit "$result"
}}
trap finish EXIT
mkdir -p {raw}
df -h ~ | tail -1
nvidia-smi --query-gpu=memory.used,memory.free --format=csv
export UV_LINK_MODE=hardlink CUDA_HOME=/usr/local/cuda MAX_JOBS=4 PATH=/usr/local/cuda/bin:$PATH
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
if [ ! -x {task}/.venv/bin/python ]; then ~/.local/bin/uv venv -q --python 3.12 {task}/.venv; fi
~/.local/bin/uv pip install -q --offline --python {task}/.venv/bin/python numpy ninja 'torch=={args.torch}' || \\
  ~/.local/bin/uv pip install -q --python {task}/.venv/bin/python numpy ninja 'torch=={args.torch}'
export PATH=$PWD/.venv/bin:$PATH
py={task}/.venv/bin/python
mkdir -p {run}/build
$py -c "import sys; sys.path.insert(0, '{src}/scripts/navsim/gpumem'); import bench; print(bench.build_reference('{src}', '{run}/build/libnavmemref.so'))" > {raw}/reference-build.txt
$py {src}/scripts/navsim/gpumem/bench.py metadata --output {raw}/metadata.json
{parity}
$py {script} speed --build {run}/build --policy {task}/weights/last.pt --agents {agents} --precisions {args.precisions} \\
  --warmup {args.warmup} --repeats {args.repeats} --fill-workers {args.fill_workers} \\
  --vram-limit-gib {args.gpu_mem - .5} --output {raw}/speed.json --captures {capture_list}
nvidia-smi --query-gpu=memory.used,memory.free --format=csv > {raw}/vram-after.txt
(cd {raw} && sha256sum -- *.json *.txt > SHA256SUMS)
$py - <<'SUMMARY'
import sys
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0, '{src}/scripts/navsim/stand')
import loop
args = SimpleNamespace(host={host!r}, gq_class={args.gq_class!r}, mem={args.mem}, gpu_mem={args.gpu_mem})
loop.summarise(Path('{raw}'), '{NAS}/{stamp}', args, output=Path('{raw}/passage.json'), stamp='{stamp}')
SUMMARY
df -h ~ | tail -1
"""
    # The explicit archive publishes raw data to the research NAS path. gq's
    # archive owns the run directory as a fallback if that publication fails;
    # successful publication and cleanup leave that directory empty.
    job = [str(ROOT / "scripts/gq"), "--host", host, "submit", "--class", args.gq_class, "--threads",
           str(DELAY_THREADS + 2), "--mem", f"{args.mem}G", "--gpu-mem", f"{args.gpu_mem}G",
           "--max", str(args.max_seconds), "--name", "navsim-stand-passage", "--archive", run,
           "--", "bash", "-lc", body]
    submitted = subprocess.run(job, check=True, text=True, capture_output=True)
    job_id = submitted.stdout.strip()
    print(f"gq submitted {job_id}; local raw {raw}; NAS {NAS}/{stamp}/passage.json", flush=True)
    return job_id


def summarise(local, nas, args, *, output=REPORT, stamp=None):
    metadata = json.loads((local / "metadata.json").read_text())
    report = json.loads(output.read_text()) if output.exists() else {}
    report.update(status="measured", method=__doc__.strip(), contract="scripts/navsim/stand/contract.py")
    run = dict(stamp=stamp or local.name.rsplit("-", 1)[-1], raw_dir=nas, gpu=metadata["gpu"], torch=metadata["torch"],
               torch_cuda=metadata["torch_cuda"], free_vram_mib_before=metadata["free_vram_mib"],
               vram_after=(local / "vram-after.txt").read_text().strip().splitlines()[-1],
               gq=dict(host=args.host, cls=args.gq_class, threads=DELAY_THREADS + 2, mem_gib=args.mem,
                       gpu_mem_gib=args.gpu_mem),
               raw_checksums=dict(path=f"{nas}/SHA256SUMS", sha256=bench.sha256(local / "SHA256SUMS")))
    if (local / "parity.json").exists():
        parity = json.loads((local / "parity.json").read_text())
        report["parity"] = dict(gpu=metadata["gpu"][0], raw=f"{nas}/parity.json",
                                **{k: v for k, v in parity.items() if k != "captures"},
                                captures=len(parity["captures"]))
    if (local / "passage-parity.json").exists():
        report["passage_parity"] = dict(raw=f"{nas}/passage-parity.json",
                                         **json.loads((local / "passage-parity.json").read_text()))
    speed = json.loads((local / "speed.json").read_text())
    run["cells"] = [{k: v for k, v in c.items() if not k.endswith("per_repeat")}
                    | {k: {kk: vv for kk, vv in v.items() if kk != "per_repeat"} for k, v in c.items()
                       if k.startswith("loop_")} for c in speed["cells"]]
    report.setdefault("runs", []).append(run)
    report["headline"] = headline(report)
    bench.write_json(output, report)
    print(f"wrote {output}", flush=True)


def headline(report):
    """Latest cell per (precision, N) over the runs; a run's note says how its code differed."""
    cells = {}
    for run in report["runs"]:
        for c in run["cells"]:
            cells[(c["precision"], c["agents"])] = (run, c)
    rows = []
    for (precision, n), (run, c) in sorted(cells.items()):
        row = dict(agents=n, precision=precision, run=run["stamp"], status=c["status"])
        if run.get("note"):
            row["note"] = run["note"]
        if c["status"] == "ok":
            row.update(gpu_us_per_decision=c["gpu_us_per_decision"]["median"],
                       gpu_parts_us_per_decision=c["gpu_parts_us_per_decision"],
                       loop_replay_decisions_per_second=c["loop_replay"]["decisions_per_second"]["median"],
                       loop_delay_decisions_per_second=c["loop_delay"]["decisions_per_second"]["median"],
                       replay_ms_per_half_step=c["replay_fill_ms_per_half_step"],
                       delay_ms_per_half_step=c["loop_delay"]["cpu_side_ms_per_half_step"],
                       peak_reserved_gib=c["peak_reserved_bytes"] / 2 ** 30, process_vram_mib=c["process_vram_mib"])
        rows.append(row)
    parity = report.get("parity", {})
    return dict(gpu=report["runs"][-1]["gpu"][0],
                parity_fp32_vs_engine=parity.get("fp32", {}).get("vs_engine_first_pass"),
                fp16_vs_fp32=parity.get("fp16_graph", {}).get("vs_fp32_first_pass", {}).get("equal_fraction"),
                speed=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    agents = lambda value: [int(v) for v in value.split(",") if v]  # noqa: E731
    p = sub.add_parser("parity")
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--reference", type=Path, required=True)
    p.add_argument("--weights", type=Path, required=True, help="policy.navbc for NavPolicy::encode")
    p.add_argument("--policy", type=Path, required=True, help="last.pt of the same net")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--captures", nargs="+", required=True)
    for name in ("speed", "cell"):
        p = sub.add_parser(name)
        p.add_argument("--build", type=Path, required=True)
        p.add_argument("--policy", type=Path, required=True)
        p.add_argument("--agents", type=agents if name == "speed" else int, required=True)
        if name == "speed":
            p.add_argument("--precisions", type=lambda v: v.split(","), default=["fp16", "fp32"])
        else:
            p.add_argument("--precision", choices=("fp16", "fp32"), default="fp16")
        p.add_argument("--warmup", type=int, default=3)
        p.add_argument("--repeats", type=int, default=20)
        p.add_argument("--fill-workers", type=int, default=8)
        p.add_argument("--vram-limit-gib", type=float, default=0)
        p.add_argument("--output", type=Path, required=True)
        p.add_argument("--captures", nargs="+", required=True)
    p = sub.add_parser("remote")
    p.add_argument("--host", choices=("ryzen",), default="ryzen", help="RTX 3080, next to the CPU part")
    p.add_argument("--gq-class", choices=("quick", "quiet"), default="quick")
    p.add_argument("--agents", type=agents, default=[2048, 3072])
    p.add_argument("--precisions", default="fp16,fp32", help="fp32 runs at the first N only")
    p.add_argument("--warmup", type=int, default=3)
    p.add_argument("--repeats", type=int, default=20)
    p.add_argument("--fill-workers", type=int, default=8)
    p.add_argument("--gpu-mem", type=int, default=9, help="GiB declared to gq; the allocator gets 0.5 less")
    p.add_argument("--mem", type=int, default=6, help="GiB of RAM declared to gq")
    p.add_argument("--max-seconds", type=int, default=3600)
    p.add_argument("--torch", default="2.14.1")
    p.add_argument("--skip-parity", action="store_true")
    args = parser.parse_args()
    if getattr(args, "warmup", 3) < 3 or getattr(args, "repeats", 20) < 20:
        parser.error("at least 3 warmups and 20 repeats")
    dict(parity=parity_command, speed=speed_command, cell=cell_command, remote=remote_command)[args.command](args)


if __name__ == "__main__":
    main()
