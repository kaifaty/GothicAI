"""Ground motion of the nav-sim stand agents through ctypes (library: stand/native, task S2).

The world of ``eye.EyeWorld`` also carries a Jolt CharacterVirtual body per
agent. One ``step`` is one decision of every agent: its keys pick the 100 ms
translation from the root-motion table (``motion_table.py``: current keys,
0-3 previous decisions, age bin), the turn is code (90 deg/s without a weapon,
``npckeys.cpp``), and the body moves over the static dump in three ticks of
33/33/34 ms in the engine's order: movement first, then the turn. Vertical
motion comes from the physics, never from the table.

Tick outputs have the fields and units of ``StepBatch`` ``pose_*``
(``contract.py``): feet position cm, forward, game ms, limits (step cm, max
slope deg, body cm), right-aligned in three slots, plus flags and the yaw.

Usage::

    with eye.EyeWorld(static_bin, agents=n, threads=8) as world:
        motion = Motion(world, table=load_table("table-full.json"))
        motion.set_state(position, yaw, game_ms, history, held)
        motion.set_keys(keys)
        ticks = motion.step()
"""
from __future__ import annotations

import ctypes
from dataclasses import dataclass
import json
from pathlib import Path
from typing import NamedTuple

import numpy as np

try:
    from . import eye
except ImportError:  # run as a script from scripts/navsim/stand
    import eye  # type: ignore[no-redef]

TICKS = 3
SUPPORTED, BLOCKED, FALLING, STEEP, UNKNOWN_KEYS, STEPPED = 1, 2, 4, 8, 16, 32
FLAGS = {"supported": SUPPORTED, "blocked": BLOCKED, "falling": FALLING, "steep": STEEP,
         "unknown_keys": UNKNOWN_KEYS, "stepped": STEPPED}
MISSING = 54  # motion_table.key_ids: no previous decision
AGE_LOWER_STEPS = np.array([0, 1, 2, 3, 5, 10, 20, 50])  # motion_table.AGE_LOWER_STEPS


class Params(ctypes.Structure):
    _fields_ = [(name, ctypes.c_float) for name in
                ("radius_cm", "height_cm", "step_cm", "slide_angle_deg", "body_cm", "turn_deg_s", "mass_kg")]


class _Ticks(ctypes.Structure):
    _fields_ = [(name, ctypes.c_void_p) for name in ("position", "forward", "limits", "ms", "flags", "yaw")]


@dataclass
class Ticks:
    """Tick outputs of several agents, right-aligned: [agents, 3(, 3)].

    The library writes through raw pointers, so ``check`` verifies every array's
    dtype, exact shape, C order and writeability before a call."""
    position: np.ndarray
    forward: np.ndarray
    limits: np.ndarray
    ms: np.ndarray
    flags: np.ndarray
    yaw: np.ndarray

    @classmethod
    def empty(cls, agents: int) -> "Ticks":
        vec = lambda: np.zeros((agents, TICKS, 3), np.float32)  # noqa: E731
        return cls(vec(), vec(), vec(), np.zeros((agents, TICKS), np.uint32),
                   np.zeros((agents, TICKS), np.uint8), np.zeros((agents, TICKS), np.float32))

    @property
    def agents(self) -> int:
        return len(self.ms)

    def check(self, agents: int):
        """Raises ValueError unless every field is a writeable C-ordered array for ``agents`` agents."""
        for name, (dtype, tail) in TICK_FIELDS.items():
            _output(getattr(self, name), dtype, (agents, *tail), f"ticks.{name}")

    def _struct(self, agents: int) -> _Ticks:
        self.check(agents)
        out = _Ticks()
        for name, _ in _Ticks._fields_:
            setattr(out, name, getattr(self, name).ctypes.data)
        return out


TICK_FIELDS = {"position": (np.float32, (TICKS, 3)), "forward": (np.float32, (TICKS, 3)),
               "limits": (np.float32, (TICKS, 3)), "ms": (np.uint32, (TICKS,)), "flags": (np.uint8, (TICKS,)),
               "yaw": (np.float32, (TICKS,))}


def _output(array, dtype, shape, name: str):
    """An array the library writes into: exactly this dtype and shape, C-contiguous, writeable."""
    if not isinstance(array, np.ndarray):
        raise ValueError(f"{name} must be a numpy array")
    if array.dtype != np.dtype(dtype):
        raise ValueError(f"{name} must be {np.dtype(dtype)}, got {array.dtype}")
    if array.shape != tuple(shape):
        raise ValueError(f"{name} must have shape {tuple(shape)}, got {array.shape}")
    if not array.flags.c_contiguous or not array.flags.writeable:
        raise ValueError(f"{name} must be C-contiguous and writeable")


class State(NamedTuple):
    position: np.ndarray  # float32 [n, 3] feet cm
    yaw: np.ndarray       # float32 [n] degrees
    game_ms: np.ndarray   # uint64 [n]
    flags: np.ndarray     # uint8 [n] of the last tick
    table_cm: np.ndarray  # float32 [n, 2] forward/right of the last decision
    level: np.ndarray     # int8 [n] table level of the last decision, -2: none


def _bind(lib: ctypes.CDLL) -> ctypes.CDLL:
    if getattr(lib, "_navsim_motion", False):
        return lib
    p, i32, i64 = ctypes.c_void_p, ctypes.c_int32, ctypes.c_int64
    signatures = {
        "navsim_motion_default_params": (None, [ctypes.POINTER(Params)]),
        "navsim_motion_init": (i32, [p, ctypes.POINTER(Params), i32]),
        "navsim_motion_shards": (i32, [p]),
        "navsim_motion_table": (i32, [p, i32, i64, p, p]),
        "navsim_motion_set_state": (i32, [p, i32, i32, p, p, p, p, p]),
        "navsim_motion_set_yaw": (i32, [p, i32, i32, p]),
        "navsim_motion_set_speed": (i32, [p, i32, i32, p]),
        "navsim_motion_set_keys": (i32, [p, i32, i32, p]),
        "navsim_motion_step": (i32, [p, i32, i32, p, ctypes.POINTER(_Ticks)]),
        "navsim_motion_read": (i32, [p, i32, i32, p, p, p, p, p, p]),
    }
    for name, (restype, argtypes) in signatures.items():
        function = getattr(lib, name)
        function.restype, function.argtypes = restype, argtypes
    lib._navsim_motion = True
    return lib


def default_params(library=None) -> Params:
    lib = _bind(eye.load_library(library))
    params = Params()
    lib.navsim_motion_default_params(ctypes.byref(params))
    return params


def codes(keys) -> np.ndarray:
    """motion_table.key_ids action code of keys [..., 4] (forward, strafe, turn, walk)."""
    k = np.asarray(keys, np.int64)
    return (((k[..., 0] + 1) * 3 + k[..., 1] + 1) * 3 + k[..., 2] + 1) * 2 + k[..., 3]


def ages(keys) -> np.ndarray:
    """Decisions since the keys last changed, as motion_table.key_ids counts them."""
    keys = np.asarray(keys)
    n = len(keys)
    changed = np.r_[True, np.any(keys[1:] != keys[:-1], axis=1)] if n else np.zeros(0, bool)
    index = np.arange(n)
    return index - np.maximum.accumulate(np.where(changed, index, 0))


def history_at(keys, index) -> tuple[np.ndarray, np.ndarray]:
    """History arguments of ``set_state`` before decision ``index`` of an episode's keys [n, 4].

    ``index`` may be an array. Returns int8 [m, 3, 4] (missing rows have walk -1)
    and the int32 age the previous keys had at their decision."""
    keys = np.asarray(keys, np.int8)
    index = np.atleast_1d(np.asarray(index, np.int64))
    age = ages(keys)
    history = np.full((len(index), 3, 4), -1, np.int8)
    for h in range(3):
        previous = index - 1 - h
        valid = previous >= 0
        history[valid, h] = keys[previous[valid]]
    held = np.where(index >= 1, age[np.maximum(index - 1, 0)], 0).astype(np.int32)
    return history, held


def load_table(path: str | Path) -> dict[int, tuple[np.ndarray, np.ndarray]]:
    """Levels of a motion table (``table-full.json`` or a results.json ``table``):
    {level: (packed keys int64 [n], forward/right cm float32 [n, 2])}."""
    data = json.loads(Path(path).read_text())
    table = data.get("table", data)
    levels = {}
    for level, rows in table["levels"].items():
        rows = np.asarray(rows, np.float64).reshape(-1, 6)
        levels[int(level)] = (rows[:, 0].astype(np.int64), np.ascontiguousarray(rows[:, 2:4], np.float32))
    return levels


def _array(values, dtype, shape) -> np.ndarray:
    array = np.ascontiguousarray(values, dtype=dtype)
    if array.shape != shape:
        raise ValueError(f"expected shape {shape}, got {array.shape}")
    return array


class Motion:
    """CharacterVirtual bodies of every agent of an ``eye.EyeWorld``."""

    def __init__(self, world: "eye.EyeWorld", table=None, shards: int | None = None, params: Params | None = None):
        self.world = world
        self._lib = _bind(world._lib)
        self.agents = world.agents
        shards = 112 if shards is None else shards  # divisible by 1, 2, 4, 7, 8, 14, 16, 28 workers
        result = self._lib.navsim_motion_init(world._handle, None if params is None else ctypes.byref(params), shards)
        self._check(result, "init")
        self.shards = int(self._lib.navsim_motion_shards(world._handle))
        if table is not None:
            self.set_table(load_table(table) if isinstance(table, (str, Path)) else table)

    def _check(self, result, name):
        if result < 0:
            raise RuntimeError(f"motion {name}: {self._lib.navsim_last_error(self.world._handle).decode()}")
        return result

    def set_table(self, levels: dict[int, tuple[np.ndarray, np.ndarray]]):
        for level, (keys, values) in levels.items():
            keys = np.ascontiguousarray(keys, np.int64)
            if keys.ndim != 1:
                raise ValueError("table keys must be [n]")
            values = _array(values, np.float32, (len(keys), 2))
            self._check(self._lib.navsim_motion_table(self.world._handle, int(level), len(keys), keys.ctypes.data,
                                                      values.ctypes.data), "table")

    def set_state(self, position, yaw, game_ms, history=None, held=None, first: int = 0):
        n = len(game_ms)
        position = _array(position, np.float32, (n, 3))
        yaw = _array(yaw, np.float32, (n,))
        ms = _array(game_ms, np.uint64, (n,))
        history = None if history is None else _array(history, np.int8, (n, 3, 4))
        held = None if held is None else _array(held, np.int32, (n,))
        self._check(self._lib.navsim_motion_set_state(
            self.world._handle, first, n, position.ctypes.data, yaw.ctypes.data, ms.ctypes.data,
            None if history is None else history.ctypes.data, None if held is None else held.ctypes.data), "set_state")

    def set_yaw(self, yaw, first: int = 0):
        yaw = np.ascontiguousarray(yaw, np.float32)
        if yaw.ndim != 1:
            raise ValueError("yaw must be [n]")
        self._check(self._lib.navsim_motion_set_yaw(self.world._handle, first, len(yaw), yaw.ctypes.data), "set_yaw")

    def set_speed(self, run_walk, first: int = 0):
        """The NPCs' pace against the table, [n, 2]: run, walk (1 = the table)."""
        run_walk = np.ascontiguousarray(run_walk, np.float32)
        if run_walk.ndim != 2 or run_walk.shape[1] != 2:
            raise ValueError("speed must be [n, 2]: run, walk")
        self._check(self._lib.navsim_motion_set_speed(self.world._handle, first, len(run_walk), run_walk.ctypes.data),
                    "set_speed")

    def set_keys(self, keys, first: int = 0):
        keys = np.ascontiguousarray(keys, np.int8)
        if keys.ndim != 2 or keys.shape[1] != 4:
            raise ValueError("keys must be [n, 4]: forward, strafe, turn, walk")
        self._check(self._lib.navsim_motion_set_keys(self.world._handle, first, len(keys), keys.ctypes.data),
                    "set_keys")

    def step(self, first: int = 0, count: int | None = None, ticks=None, out: Ticks | None = None) -> Ticks:
        """One decision of agents [first, first+count); ``out`` holds count agents."""
        count = self.agents - first if count is None else count
        out = Ticks.empty(count) if out is None else out
        struct = out._struct(count)  # before the call: the library trusts these buffers
        ticks = None if ticks is None else _array(ticks, np.uint8, (count,))
        self._check(self._lib.navsim_motion_step(self.world._handle, first, count,
                                                 None if ticks is None else ticks.ctypes.data,
                                                 ctypes.byref(struct)), "step")
        return out

    def read(self, first: int = 0, count: int | None = None) -> State:
        count = self.agents - first if count is None else count
        state = State(np.zeros((count, 3), np.float32), np.zeros(count, np.float32), np.zeros(count, np.uint64),
                      np.zeros(count, np.uint8), np.zeros((count, 2), np.float32), np.zeros(count, np.int8))
        self._check(self._lib.navsim_motion_read(self.world._handle, first, count,
                                                 *(a.ctypes.data for a in state)), "read")
        return state
