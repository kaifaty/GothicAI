"""The nav-sim stand's world and eye through ctypes (library: stand/native).

The library loads a NAVSTAT1 static dump into Embree and keeps N agents, each
with the engine's NavigationEye core (layout 250, walking gaze). The caller
sets every agent's head pose and game time, then one ``update`` runs all eyes
on the library's thread pool. Slots come out agent-major in the layout of the
gpumem eye inputs (gpumem/memory_kernels.h, ``Ctx::rayOrigin..rayTime``): 476
per agent, the 238 fresh rays followed by the cached far rays.

Build (Ryzen does it in ``stand-remote.sh``)::

    cmake -S scripts/navsim/stand/native -B scripts/navsim/stand/native/build \\
          -DCMAKE_PREFIX_PATH=<embree-4.4.0> && cmake --build scripts/navsim/stand/native/build

The library is taken from ``NAVSIM_STAND_LIB`` or from ``native/build``.
"""
from __future__ import annotations

import ctypes
from dataclasses import dataclass, fields
import os
from pathlib import Path
import sys
from typing import NamedTuple

import numpy as np

HERE = Path(__file__).resolve().parent
RAYS, SLOTS = 238, 476
KINDS = ("none", "world", "static_vob", "movable_vob", "npc", "water")
# Slot fields gpumem reads, by its buffer names.
GPUMEM = {"rayOrigin": "origin", "rayPoint": "point", "rayDistance": "distance", "rayNormalY": "normal_y",
          "rayKind": "kind", "rayHit": "hit", "rayTime": "time"}


class _Slots(ctypes.Structure):
    _fields_ = [(name, ctypes.c_void_p) for name in
                ("origin", "point", "distance", "normal_y", "kind", "hit", "time", "direction", "complete")]


@dataclass
class Slots:
    """Eye output of several agents; arrays are [agents, 476(, 3)]."""
    origin: np.ndarray
    point: np.ndarray
    distance: np.ndarray
    normal_y: np.ndarray
    kind: np.ndarray
    hit: np.ndarray
    time: np.ndarray
    direction: np.ndarray
    complete: np.ndarray

    @classmethod
    def empty(cls, agents: int) -> "Slots":
        vec = lambda: np.zeros((agents, SLOTS, 3), np.float32)  # noqa: E731
        one = lambda dtype: np.zeros((agents, SLOTS), dtype)  # noqa: E731
        return cls(vec(), vec(), one(np.float32), one(np.float32), one(np.uint8), one(np.uint8),
                   one(np.uint32), vec(), one(np.uint8))

    @property
    def agents(self) -> int:
        return len(self.distance)

    def gpumem(self) -> dict[str, np.ndarray]:
        """Flat views named like the gpumem eye buffers (``gpumem.layout(...)['eye']``)."""
        return {name: getattr(self, field).reshape(-1) for name, field in GPUMEM.items()}

    def _struct(self) -> _Slots:
        out = _Slots()
        for f in fields(self):
            array = getattr(self, f.name)
            if not array.flags.c_contiguous:
                raise ValueError(f"slots.{f.name} must be C-contiguous")
            setattr(out, f.name, array.ctypes.data)
        return out


class Update(NamedTuple):
    updated: np.ndarray      # uint8 [agents]
    far_updated: np.ndarray  # uint8 [agents]
    casts: np.ndarray        # uint32 [agents]
    total_casts: int


def library_path() -> Path | None:
    """The built library, or None (then the stand cannot run here)."""
    explicit = os.environ.get("NAVSIM_STAND_LIB")
    if explicit:
        return Path(explicit) if Path(explicit).is_file() else None
    name = "libnavsim_stand.dylib" if sys.platform == "darwin" else "libnavsim_stand.so"
    path = HERE / "native" / "build" / name
    return path if path.is_file() else None


def load_library(path: str | Path | None = None) -> ctypes.CDLL:
    path = Path(path) if path else library_path()
    if path is None:
        raise FileNotFoundError("navsim stand library is not built: scripts/navsim/stand/native (or NAVSIM_STAND_LIB)")
    lib = ctypes.CDLL(str(path))
    p, i32, i64, u8p = ctypes.c_void_p, ctypes.c_int32, ctypes.c_int64, ctypes.c_void_p
    signatures = {
        "navsim_eye_rays": (i32, []), "navsim_eye_slots": (i32, []),
        "navsim_world_create": (p, [ctypes.c_char_p, i32, i32, ctypes.c_char_p, ctypes.c_size_t]),
        "navsim_world_destroy": (None, [p]), "navsim_world_agents": (i32, [p]),
        "navsim_world_triangles": (i64, [p]), "navsim_last_error": (ctypes.c_char_p, [p]),
        "navsim_set_threads": (i32, [p, i32, p]),
        "navsim_set_poses": (i32, [p, i32, i32, p, p, p, p, p, p]),
        "navsim_set_poses_yaw": (i32, [p, i32, i32, p, p, p, p]),
        "navsim_reset_eyes": (i32, [p, i32, i32]),
        "navsim_eye_update": (i64, [p, ctypes.POINTER(_Slots), u8p, u8p, p]),
        "navsim_eye_read": (i32, [p, i32, i32, ctypes.POINTER(_Slots)]),
    }
    for name, (restype, argtypes) in signatures.items():
        function = getattr(lib, name)
        function.restype, function.argtypes = restype, argtypes
    if (lib.navsim_eye_rays(), lib.navsim_eye_slots()) != (RAYS, SLOTS):
        raise RuntimeError("navsim stand library has another eye layout")
    return lib


def _array(values, dtype, shape) -> np.ndarray:
    array = np.ascontiguousarray(values, dtype=dtype)
    if array.shape != shape:
        raise ValueError(f"expected shape {shape}, got {array.shape}")
    return array


class EyeWorld:
    """Static world and the eyes of ``agents`` agents."""

    def __init__(self, static_bin: str | Path, agents: int, threads: int = 1, library: str | Path | None = None):
        self._lib = load_library(library)
        error = ctypes.create_string_buffer(512)
        self._handle = self._lib.navsim_world_create(str(static_bin).encode(), agents, threads, error, len(error))
        if not self._handle:
            raise RuntimeError(f"navsim_world_create: {error.value.decode()}")
        self.agents = agents
        self.triangles = int(self._lib.navsim_world_triangles(self._handle))

    def close(self):
        if getattr(self, "_handle", None):
            self._lib.navsim_world_destroy(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        self.close()

    def _check(self, result, name):
        if result < 0:
            raise RuntimeError(f"{name}: {self._lib.navsim_last_error(self._handle).decode()}")
        return result

    def set_threads(self, threads: int, cpus=None):
        pinned = None if cpus is None else _array(cpus, np.int32, (threads,))
        self._check(self._lib.navsim_set_threads(self._handle, threads,
                                                 None if pinned is None else pinned.ctypes.data), "set_threads")

    def set_poses(self, origin, forward, right, up, position, game_ms, first: int = 0):
        n = len(game_ms)
        arrays = [_array(v, np.float32, (n, 3)) for v in (origin, forward, right, up, position)]
        ms = _array(game_ms, np.uint64, (n,))
        self._check(self._lib.navsim_set_poses(self._handle, first, n, *(a.ctypes.data for a in arrays),
                                               ms.ctypes.data), "set_poses")

    def set_poses_yaw(self, origin, position, yaw_deg, game_ms, first: int = 0):
        """Neutral head frame from body yaw, like the engine's perception replay."""
        n = len(game_ms)
        origin, position = (_array(v, np.float32, (n, 3)) for v in (origin, position))
        yaw, ms = _array(yaw_deg, np.float32, (n,)), _array(game_ms, np.uint64, (n,))
        self._check(self._lib.navsim_set_poses_yaw(self._handle, first, n, origin.ctypes.data,
                                                   position.ctypes.data, yaw.ctypes.data, ms.ctypes.data),
                    "set_poses_yaw")

    def reset(self, first: int = 0, count: int | None = None):
        count = self.agents - first if count is None else count
        self._check(self._lib.navsim_reset_eyes(self._handle, first, count), "reset")

    def update(self, out: Slots | None = None) -> Update:
        """One eye step of every agent; ``out`` (all agents) receives the slots."""
        if out is not None and out.agents != self.agents:
            raise ValueError("out must hold every agent")
        updated, far = np.zeros(self.agents, np.uint8), np.zeros(self.agents, np.uint8)
        casts = np.zeros(self.agents, np.uint32)
        struct = None if out is None else ctypes.byref(out._struct())
        total = self._check(self._lib.navsim_eye_update(self._handle, struct, updated.ctypes.data,
                                                        far.ctypes.data, casts.ctypes.data), "update")
        return Update(updated, far, casts, int(total))

    def read(self, first: int = 0, count: int | None = None, out: Slots | None = None) -> Slots:
        count = self.agents - first if count is None else count
        out = Slots.empty(count) if out is None else out
        if out.agents != count:
            raise ValueError("out must hold count agents")
        self._check(self._lib.navsim_eye_read(self._handle, first, count, ctypes.byref(out._struct())), "read")
        return out
