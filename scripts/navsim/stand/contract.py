"""Contract between the stand's CPU side and its GPU loop (docs/nav-sim.md, "Устройство").

A CPU side serves a fixed set of agents; the loop holds two of them, one per
half. Phase A, ``reset()`` or ``step(actions)``, returns a ``StepBatch`` with
world ticks, eye, rewards, episode flags and passage requests. The GPU updates
memory once and checks those requests. Phase B, ``finish(verdicts)``, consumes
the uint8 verdicts in request order (Unknown=0, Free=1, Closed=2), then returns
``FinishBatch`` with roads encoded in the vector and teacher labels, or another
``PassageQueries``. Further rounds read the same memory: no ticks or resets.
All returned buffers stay valid until the next CPU-side call. One step is one
decision (100 ms); even zero queries require ``finish`` with an empty array.

Fields, per agent (struct of arrays, every field one contiguous [n, ...] block):

* rays: the eye's full output after this decision's eye refresh, 476 slots in
  the layout of the gpumem ray slots (memory_kernels.h, ``Ctx`` rayOrigin..
  rayTime): origin and point in cm, distance, normal y, kind, hit, sample time
  in game ms. The memory takes them at the last tick of the decision.
* poses of the three world ticks (33 ms) that led to this decision: position,
  forward, game ms, limits (step, slope, body). Valid ticks are right-aligned:
  ``ticks`` = 3 normally, 1 at the first decision of an episode; tick 2 is
  always the eye tick. Earlier ticks of a short decision are ignored.
* ``first``: this decision opens an episode. The loop clears the agent's
  memory (as the engine's NavigationMemory::reset) and its GRU state first.
  A first decision has ``ticks`` = 1.
* reward components of the transition into this decision (REWARDS, unscaled;
  weights are nav_ppo.REWARD_WEIGHTS) and ``terminated``/``truncated`` of that
  transition. When either is set the episode ended and, in the same step, the
  side started the next one: this batch is already its first decision.

Actions passed to ``step`` are uint8 [n, 4]: the class index of each head in
NavPolicy.head_sizes order (forward, strafe, turn, maneuver). They are valid
only during the call.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol

import numpy as np

SLOTS = 2 * 238
TICKS = 3
VECTOR = 2167
HEADS = 4
REWARDS = ("progress_cm", "arrived", "blocked", "fall_damage", "death", "jump", "climb", "time")
ALIGN = 256

# name, dtype, per-agent shape
FIELDS = (
    ("ray_origin", np.float32, (SLOTS, 3)),
    ("ray_point", np.float32, (SLOTS, 3)),
    ("ray_distance", np.float32, (SLOTS,)),
    ("ray_normal_y", np.float32, (SLOTS,)),
    ("ray_time", np.uint32, (SLOTS,)),
    ("ray_kind", np.uint8, (SLOTS,)),
    ("ray_hit", np.uint8, (SLOTS,)),
    ("pose_pos", np.float32, (TICKS, 3)),
    ("pose_fwd", np.float32, (TICKS, 3)),
    ("pose_limits", np.float32, (TICKS, 3)),
    ("pose_ms", np.uint32, (TICKS,)),
    ("ticks", np.uint8, ()),
    ("first", np.uint8, ()),
    ("reward", np.float32, (len(REWARDS),)),
    ("terminated", np.uint8, ()),
    ("truncated", np.uint8, ()),
)
FINISH_FIELDS = (("vector", np.float32, (VECTOR,)),
                 ("teacher", np.uint8, (HEADS,)), ("teacher_valid", np.uint8, (HEADS,)))
# Ray fields and the gpumem slot buffer each one is copied into, byte for byte.
RAY_SLOTS = dict(ray_origin="rayOrigin", ray_point="rayPoint", ray_distance="rayDistance",
                 ray_normal_y="rayNormalY", ray_time="rayTime", ray_kind="rayKind", ray_hit="rayHit")


@dataclass(frozen=True)
class Field:
    name: str
    dtype: np.dtype
    shape: tuple
    offset: int
    nbytes: int


def layout(n: int, entries=FIELDS) -> tuple[dict[str, Field], int]:
    """Byte layout of a batch of n agents: fields in FIELDS order, 256-byte aligned."""
    fields, offset = {}, 0
    for name, dtype, shape in entries:
        dtype = np.dtype(dtype)
        nbytes = n * int(np.prod(shape, dtype=np.int64)) * dtype.itemsize
        fields[name] = Field(name, dtype, (n, *shape), offset, nbytes)
        offset += -(-nbytes // ALIGN) * ALIGN
    return fields, max(offset, ALIGN)


class _Buffer:
    """One pinned byte buffer with a numpy view per field (``batch.ray_origin`` ...)."""

    def __init__(self, n: int, pinned: bool = False, entries=FIELDS):
        self.n = n
        self.fields, self.nbytes = layout(n, entries)
        self.tensor = None
        if pinned:
            import torch
            self.tensor = torch.zeros(self.nbytes, dtype=torch.uint8, pin_memory=True)
            self.raw = self.tensor.numpy()
        else:
            self.raw = np.zeros(self.nbytes, np.uint8)
        for f in self.fields.values():
            view = self.raw[f.offset: f.offset + f.nbytes].view(f.dtype).reshape(f.shape)
            setattr(self, f.name, view)


class PassageQueries(_Buffer):
    """Ordered queries for local agent ids; coordinates and distances are cm.

    Each segment is [from.x, from.y, from.z, to.x, to.y, to.z, corridor,
    footprint]. Only the first ``count`` rows are valid. A side may request up
    to 34 per agent in a round, covering KnownRoads' rebuild in one round.
    """

    def __init__(self, n: int, pinned: bool = False):
        super().__init__(34 * n, pinned, (("agents", np.int32, ()), ("segments", np.float32, (8,))))
        self.count = 0

    def check(self, agents):
        if not 0 <= self.count <= self.n or np.any((self.agents[:self.count] < 0)
                                                  | (self.agents[:self.count] >= agents)):
            raise ValueError("invalid passage query count or agent")


class FinishBatch(_Buffer):
    """Phase B: v5 vector [n,2167], teacher classes and per-head validity [n,4]."""

    def __init__(self, n: int, pinned: bool = False):
        super().__init__(n, pinned, FINISH_FIELDS)


class StepBatch(_Buffer):
    """Phase A; the final tick's eye is consumed before ``queries`` are checked."""

    def __init__(self, n: int, pinned: bool = False):
        super().__init__(n, pinned)
        self.queries = PassageQueries(n, pinned)

    def check(self) -> None:
        """The invariants the GPU loop relies on; raises ValueError."""
        if np.any((self.ticks < 1) | (self.ticks > TICKS)):
            raise ValueError("ticks must be 1..3")
        if np.any(self.first.astype(bool) & (self.ticks != 1)):
            raise ValueError("a first decision has one tick")
        ends = (self.terminated | self.truncated).astype(bool)
        if np.any(ends & ~self.first.astype(bool)):
            raise ValueError("an ended episode must restart in the same step")
        valid = np.arange(TICKS)[None, :] >= TICKS - self.ticks[:, None].astype(np.int64)
        ms = self.pose_ms.astype(np.int64)
        if np.any(valid[:, 1:] & valid[:, :-1] & (ms[:, 1:] < ms[:, :-1])):
            raise ValueError("game time goes back inside a decision")
        self.queries.check(self.n)


class CpuSide(Protocol):
    """World, eye, roads, reward and teachers for a fixed set of agents."""

    agents: int

    def reset(self) -> StepBatch:
        """Start an episode for every agent: every row has first=1, ticks=1."""

    def step(self, actions: np.ndarray) -> StepBatch:
        """Apply uint8 [agents, 4] actions, run three ticks, return the next decision."""

    def finish(self, verdicts: np.ndarray) -> FinishBatch | PassageQueries:
        """Consume this round's verdicts; finish roads/encode/teacher or request more."""
