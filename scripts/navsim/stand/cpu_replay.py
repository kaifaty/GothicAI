"""A CpuSide that replays memory-cost.bin captures instead of simulating the world.

Each capture is cut into decisions at its readouts: in the drive records the
first decision is one tick with the eye, every later one three ticks with the
eye refresh on the last. Each decision keeps the eye's full output after it
(476 slots), so a batch is a gather over prepared tables.

Agent ``i`` replays capture ``i % C`` from a start shifted by the golden ratio,
like bench.py scale; a capture that ends wraps to its first decision and
reports ``truncated``. The vector comes from ``vectors`` (one row per decision,
for example NavPolicy::encode from navsim_memory_ref) or from a fixed random
pool. Actions are ignored: the replay is open loop.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "gpumem"))
import captures  # noqa: E402
import contract  # noqa: E402
from contract import SLOTS, TICKS, VECTOR, StepBatch  # noqa: E402

POOL = 61  # synthetic vectors; prime so agents with shifted starts do not line up


def times32(values):
    values = np.asarray(values, np.uint64)
    if values.size and values.max() >= 2 ** 32 - 1:
        raise ValueError("game time does not fit the 32-bit state")
    return values.astype(np.uint32)


class Replay:
    """Decision tables of several captures, concatenated; row g is one decision."""

    def __init__(self, records, vectors=None, seed=20261104):
        self.records = list(records)
        if any(c.slots != SLOTS for c in self.records):
            raise ValueError(f"captures must have {SLOTS} eye slots")
        rows = []
        counts = []
        for c in self.records:
            rays = np.zeros(SLOTS, captures.RAY)
            chunk, decisions = [], 0
            for u in range(c.updates):
                first, last = c.begin[u], c.begin[u + 1]
                rays[c.slot[first:last]] = c.changed[first:last]
                chunk.append(u)
                if c.views_after[u] == 0:
                    if last > first:
                        raise ValueError(f"{c.path}: update {u} refreshes the eye before the decision's last tick")
                    continue
                if len(chunk) > TICKS or (decisions == 0 and len(chunk) != 1):
                    raise ValueError(f"{c.path}: decision {decisions} has {len(chunk)} ticks")
                rows.append((c.poses[chunk], rays.copy()))
                decisions += 1
                chunk = []
            counts.append(decisions)  # trailing ticks without a readout are dropped
        self.count = np.array(counts, np.int64)
        self.first_decision = np.concatenate(([0], np.cumsum(self.count)[:-1])).astype(np.int64)
        D = len(rows)
        self.decisions = D
        self.ticks = np.array([len(p) for p, _ in rows], np.uint8)
        self.table = {name: np.zeros((D, *shape), dtype) for name, dtype, shape in contract.FIELDS
                      if name.startswith(("ray_", "pose_"))}
        for g, (poses, rays) in enumerate(rows):
            k = TICKS - len(poses)  # right-aligned: the last tick is the eye tick
            self.table["pose_pos"][g, k:] = poses["position"]
            self.table["pose_fwd"][g, k:] = poses["forward"]
            self.table["pose_ms"][g, k:] = times32(poses["ms"])
            self.table["pose_limits"][g, k:] = np.stack((poses["step"], poses["slope"], poses["body"]), 1)
            self.table["ray_origin"][g] = rays["origin"]
            self.table["ray_point"][g] = rays["point"]
            self.table["ray_distance"][g] = rays["distance"]
            self.table["ray_normal_y"][g] = rays["normal_y"]
            self.table["ray_time"][g] = times32(rays["time"])
            self.table["ray_kind"][g] = rays["kind"]
            self.table["ray_hit"][g] = rays["hit"]
        if vectors is None:
            rng = np.random.default_rng(seed)
            self.vectors, self.vector_row = rng.uniform(-1, 1, (POOL, VECTOR)).astype(np.float32), POOL
        else:
            vectors = np.ascontiguousarray(vectors, np.float32)
            if vectors.shape != (D, VECTOR):
                raise ValueError(f"vectors must be [{D}, {VECTOR}]")
            self.vectors, self.vector_row = vectors, None

    def nbytes(self):
        return sum(v.nbytes for v in self.table.values()) + self.vectors.nbytes


class ReplaySide:
    """Agents ``agents`` (global ids; the shift depends on the id only) of a Replay, as a CpuSide."""

    def __init__(self, replay: Replay, agents, shift=True, pinned=False, workers=1):
        self.replay = replay
        ids = np.asarray(agents, np.int64)
        self.ids = ids
        self.agents = len(ids)
        C = len(replay.records)
        self.record = ids % C
        if shift:
            phase = np.mod((ids // C) * 0.6180339887498949 + self.record * 0.3819660112501051, 1.0)
            self.start = (phase * replay.count[self.record]).astype(np.int64)
        else:
            self.start = np.zeros(len(ids), np.int64)
        self.cursor = self.start.copy()
        self.decision = replay.first_decision[self.record] + self.cursor
        self.batch = StepBatch(self.agents, pinned=pinned)
        self.finished = contract.FinishBatch(self.agents, pinned=pinned)
        self.pending = False
        self.sequence = 0
        self.last_verdicts = np.zeros(0, np.uint8)
        self.seconds = 0.0
        self.steps = 0
        # numpy's take releases the GIL: agent ranges fill in parallel.
        self.pool = ThreadPoolExecutor(workers) if workers > 1 else None
        bounds = np.linspace(0, self.agents, max(1, workers) + 1).astype(np.int64)
        self.ranges = [slice(a, b) for a, b in zip(bounds[:-1], bounds[1:]) if b > a]

    def reset(self) -> StepBatch:
        if self.pending:
            raise RuntimeError("finish the current decision before resetting")
        self.sequence = 0
        self.cursor = self.start.copy()
        return self._fill(np.ones(self.agents, bool), np.zeros(self.agents, bool))

    def step(self, actions=None) -> StepBatch:
        if self.pending:
            raise RuntimeError("finish the current decision before stepping")
        self.sequence += 1
        self.cursor += 1
        wrap = self.cursor >= self.replay.count[self.record]
        self.cursor[wrap] = 0
        return self._fill(wrap, wrap)

    def _fill(self, first, truncated) -> StepBatch:
        started = time.perf_counter()
        b = self.batch
        self.decision = self.replay.first_decision[self.record] + self.cursor
        if self.pool is None:
            self._gather(slice(None))
        else:
            list(self.pool.map(self._gather, self.ranges))
        b.ticks[first] = 1
        b.first[:] = first
        b.reward[:] = 0
        b.terminated[:] = 0
        b.truncated[:] = truncated
        # Speculate all rebuild candidates together: 32 starts, projection and
        # catch-up. Exactly one in 50 decisions rebuilds for each global agent.
        phase = self.ids + self.sequence
        counts = np.where(phase % 50 == 0, 34, phase % 2).astype(np.int64)
        q = b.queries
        q.count = int(counts.sum())
        owners = np.repeat(np.arange(self.agents, dtype=np.int32), counts)
        q.agents[:q.count] = owners
        ordinal = np.arange(q.count) - np.repeat(np.cumsum(counts) - counts, counts)
        segments = q.segments[:q.count]
        segments[:, :3] = b.pose_pos[owners, -1]
        segments[:, 3:6] = segments[:, :3]
        angle = (ordinal + phase[owners] % 17) * np.float32(0.61803398875)
        distance = 100 + (ordinal % 8) * 200
        segments[:, 3] += np.cos(angle) * distance
        segments[:, 5] += np.sin(angle) * distance
        segments[:, 6:] = (20, 25)  # FollowTrail::corridorCm / footprintCm, used by KnownRoads
        self.pending = True
        self.seconds += time.perf_counter() - started
        self.steps += 1
        return b

    def _gather(self, rows):
        b, r, g = self.batch, self.replay, self.decision[rows]
        for name, table in r.table.items():
            np.take(table, g, axis=0, out=getattr(b, name)[rows])
        np.take(r.ticks, g, out=b.ticks[rows])

    def finish(self, verdicts):
        started = time.perf_counter()
        verdicts = np.asarray(verdicts)
        if not self.pending or verdicts.dtype != np.uint8 or verdicts.shape != (self.batch.queries.count,):
            raise ValueError("finish needs one uint8 verdict per current query")
        if np.any(verdicts > 2):
            raise ValueError("unknown passage verdict")
        # Replay is deliberately open loop: consume and retain the actual
        # replies, while preserving the captured engine vector for net parity.
        self.last_verdicts = verdicts.copy()
        r, g = self.replay, self.decision
        np.take(r.vectors, g if r.vector_row is None else g % r.vector_row,
                axis=0, out=self.finished.vector)
        self.finished.teacher[:] = 0
        self.finished.teacher_valid[:] = 0  # captures do not contain teacher labels
        self.pending = False
        self.seconds += time.perf_counter() - started
        return self.finished

    def close(self):
        if self.pool is not None:
            self.pool.shutdown()
