"""Batched navigation memory: state layout, backends and recorded-eye tables.

``cuda`` keeps every array in torch CUDA tensors and launches memory_cuda.cu
(built once with --fmad=false, no fast math). ``host`` runs the same
memory_kernels.h on the CPU through ctypes; it exists to check the port's
logic against the engine on a machine without a GPU.
"""
from __future__ import annotations

import ctypes
from pathlib import Path
import subprocess

import numpy as np

import captures

HERE = Path(__file__).resolve().parent
NEAR, FAR = (64, 25.0), (100, 200.0)
OUTPUTS = 64 * 64 + 50 * 100
CUDA_FLAGS = ["-O3", "--fmad=false", "--prec-div=true", "--prec-sqrt=true", "--ftz=false",
              "-gencode=arch=compute_86,code=sm_86", "-std=c++20", "-lineinfo"]
HOST_FLAGS = ["-O2", "-std=c++17", "-fPIC", "-shared", "-ffp-contract=off"]
# "exact" is the port. "float-confidence" only measures the price of the
# engine's double-precision confidence; it is not bitwise.
VARIANTS = {"exact": [], "float-confidence": ["-DGM_FLOAT_CONFIDENCE"]}

POINTERS = ("posePos poseFwd poseMs poseLimits active rayOrigin rayPoint rayDistance rayNormalY rayKind "
            "rayHit rayTime bodyPos bodyFwd bodyMs bodyLimits pathCm posed journey stamps histT histP "
            "histHead histCount fresh freshCount freshPath water waterCount updated resetFlag errors mapId "
            "famMinX famMinZ famValid famWindow tileX tileZ tileGround tileChannels mask mapTileBegin "
            "mapMaskBegin").split()
GRID = "known time path height hitX hitZ minX minZ positioned".split()
TABLE = "begin slot origin point distance normalY kind hit time pos fwd ms limits".split()


class GridPtrs(ctypes.Structure):
    _fields_ = [(name, ctypes.c_void_p) for name in GRID]


class Ctx(ctypes.Structure):
    _fields_ = ([(name, ctypes.c_int64) for name in ("N", "R", "histCap", "reserved")]
                + [(name, ctypes.c_void_p) for name in POINTERS] + [("grid", GridPtrs * 2)])


class Table(ctypes.Structure):
    _fields_ = [("updates", ctypes.c_int64), ("changed", ctypes.c_int64)] + [(n, ctypes.c_void_p) for n in TABLE]


# Persistent per-agent state; scratch, eye slots and inputs are counted apart.
def layout(N: int, R: int, hist: int):
    state, scratch, eye = [], [], []
    for index, (size, _) in enumerate((NEAR, FAR)):
        cells = N * size * size
        state += [(f"g{index}.known", np.uint8, cells), (f"g{index}.time", np.uint32, 7 * cells),
                  (f"g{index}.path", np.float32, 7 * cells), (f"g{index}.height", np.float32, 5 * cells),
                  (f"g{index}.hitX", np.float32, 4 * cells), (f"g{index}.hitZ", np.float32, 4 * cells),
                  (f"g{index}.minX", np.int32, N), (f"g{index}.minZ", np.int32, N),
                  (f"g{index}.positioned", np.uint8, N)]
    state += [("bodyPos", np.float32, 3 * N), ("bodyFwd", np.float32, 3 * N), ("bodyMs", np.uint32, N),
              ("bodyLimits", np.float32, 3 * N), ("pathCm", np.float64, N), ("posed", np.uint8, N),
              ("journey", np.uint8, N), ("stamps", np.uint32, N * R), ("histT", np.uint32, N * hist),
              ("histP", np.float64, N * hist), ("histHead", np.int32, N), ("histCount", np.int32, N),
              ("mapId", np.int32, N), ("famMinX", np.int32, N), ("famMinZ", np.int32, N),
              ("famValid", np.uint8, N), ("famWindow", np.int32, N * FAR[0] ** 2)]
    scratch += [("fresh", np.uint16, N * R), ("freshCount", np.int32, N), ("freshPath", np.float32, N * R),
                ("water", np.uint16, N * R), ("waterCount", np.int32, N), ("updated", np.uint8, N),
                ("resetFlag", np.uint8, N), ("errors", np.uint32, N)]
    eye += [("posePos", np.float32, 3 * N), ("poseFwd", np.float32, 3 * N), ("poseMs", np.uint32, N),
            ("poseLimits", np.float32, 3 * N), ("active", np.uint8, N), ("rayOrigin", np.float32, 3 * N * R),
            ("rayPoint", np.float32, 3 * N * R), ("rayDistance", np.float32, N * R),
            ("rayNormalY", np.float32, N * R), ("rayKind", np.uint8, N * R), ("rayHit", np.uint8, N * R),
            ("rayTime", np.uint32, N * R)]
    return dict(state=state, scratch=scratch, eye=eye)


def bytes_of(entries, per=1):
    return sum(np.dtype(dtype).itemsize * count for _, dtype, count in entries) // per


class Backend:
    """Raw byte buffers with stable addresses on one device."""

    def __init__(self, kind: str, build: Path, variant: str = "exact"):
        self.kind, self.variant = kind, variant
        if variant not in VARIANTS:
            raise ValueError(variant)
        self.build = Path(build) / variant
        self.build.mkdir(parents=True, exist_ok=True)
        if kind == "cuda":
            import torch
            from torch.utils.cpp_extension import load
            self.torch = torch
            self.ext = load(name=f"navsim_gpumem_{variant.replace('-', '_')}", sources=[str(HERE / "memory_cuda.cu")],
                            extra_include_paths=[str(HERE)], extra_cuda_cflags=CUDA_FLAGS + VARIANTS[variant],
                            extra_cflags=["-O2"], build_directory=str(self.build), verbose=False)
            self.ctx_bytes, self.table_bytes = self.ext.ctx_bytes(), self.ext.table_bytes()
        elif kind == "host":
            library = self.build / "libgpumemhost.so"
            source = HERE / "memory_host.cpp"
            if not library.exists() or library.stat().st_mtime < max(source.stat().st_mtime,
                                                                     (HERE / "memory_kernels.h").stat().st_mtime):
                subprocess.run(["c++", *HOST_FLAGS, *VARIANTS[variant], str(source), "-o", str(library)], check=True)
            self.lib = ctypes.CDLL(str(library))
            for name in ("gm_ctx_bytes", "gm_table_bytes"):
                getattr(self.lib, name).restype = ctypes.c_int64
            self.lib.gm_update.argtypes = [ctypes.c_void_p]
            self.lib.gm_read.argtypes = [ctypes.c_void_p] * 2 + [ctypes.c_int64] + [ctypes.c_void_p] * 2
            self.lib.gm_scatter.argtypes = [ctypes.c_void_p] * 3
            self.ctx_bytes, self.table_bytes = self.lib.gm_ctx_bytes(), self.lib.gm_table_bytes()
        else:
            raise ValueError(kind)
        if (self.ctx_bytes, self.table_bytes) != (ctypes.sizeof(Ctx), ctypes.sizeof(Table)):
            raise RuntimeError("Ctx/Table layout differs between Python and C++")

    def zeros(self, dtype, count):
        nbytes = max(1, np.dtype(dtype).itemsize * int(count))
        if self.kind == "cuda":
            buffer = self.torch.zeros(nbytes, dtype=self.torch.uint8, device="cuda")
            return buffer, buffer.data_ptr()
        buffer = np.zeros(nbytes, np.uint8)
        return buffer, buffer.ctypes.data

    def upload(self, array):
        array = np.ascontiguousarray(array)
        if self.kind == "cuda":
            buffer = self.torch.from_numpy(array.reshape(-1).view(np.uint8).copy()).to("cuda")
            return buffer, buffer.data_ptr()
        buffer = array.reshape(-1).view(np.uint8).copy()
        return buffer, buffer.ctypes.data

    def download(self, buffer, dtype):
        raw = buffer.cpu().numpy() if self.kind == "cuda" else buffer
        return raw.view(dtype)

    def store(self, buffer, array):
        """Copy a numpy array into an existing buffer (same byte size)."""
        data = np.ascontiguousarray(array).reshape(-1).view(np.uint8)
        if self.kind == "cuda":
            buffer.copy_(self.torch.from_numpy(data.copy()))
        else:
            buffer[:] = data


class Memory:
    """N agents with R eye slots and a path-history ring of ``hist`` entries."""

    def __init__(self, backend: Backend, N: int, R: int, hist: int, journey, maps=(), map_ids=None):
        self.backend, self.N, self.R, self.hist = backend, N, R, hist
        self.layout = layout(N, R, hist)
        self.buffers, self.ctx = {}, Ctx(N=N, R=R, histCap=hist)
        for group in self.layout.values():
            for name, dtype, count in group:
                buffer, address = backend.zeros(dtype, count)
                self.buffers[name] = (buffer, dtype)
                if name.startswith("g"):
                    setattr(self.ctx.grid[int(name[1])], name.split(".")[1], address)
                else:
                    setattr(self.ctx, name, address)
        journey = np.broadcast_to(np.asarray(journey, np.uint8), (N,))
        backend.store(self.buffers["journey"][0], journey)
        # Concatenated finished atlases; agents index them by map id.
        tiles = [m[0] for m in maps] or [np.zeros(0, captures.TILE)]
        masks = [m[1] for m in maps] or [np.zeros(0, np.uint64)]
        tile_begin = np.cumsum([0] + [len(t) for t in tiles]).astype(np.int64)
        mask_begin = np.cumsum([0] + [len(m) for m in masks]).astype(np.int64)
        atlas = np.concatenate(tiles)
        self.atlas = {}
        for name, value in (("tileX", atlas["x"].astype(np.int32)), ("tileZ", atlas["z"].astype(np.int32)),
                            ("tileGround", atlas["ground"].astype(np.float32)),
                            ("tileChannels", atlas["channels"].astype(np.uint8)),
                            ("mask", np.concatenate(masks).astype(np.uint64)),
                            ("mapTileBegin", tile_begin), ("mapMaskBegin", mask_begin)):
            buffer, address = backend.upload(value)
            self.atlas[name] = buffer
            setattr(self.ctx, name, address)
        ids = np.full(N, -1, np.int32) if map_ids is None else np.asarray(map_ids, np.int32)
        backend.store(self.buffers["mapId"][0], ids)
        self.address = ctypes.addressof(self.ctx)

    def get(self, name):
        buffer, dtype = self.buffers[name]
        return self.backend.download(buffer, dtype)

    def bytes_per_agent(self):
        state = self.layout["state"]
        groups = {
            "grids_near": [e for e in state if e[0].startswith("g0.")],
            "grids_far": [e for e in state if e[0].startswith("g1.")],
            "path_history": [e for e in state if e[0] in ("histT", "histP", "histHead", "histCount")],
            "ray_stamps": [e for e in state if e[0] == "stamps"],
            "familiar_window": [e for e in state if e[0] in ("famWindow", "famMinX", "famMinZ", "famValid", "mapId")],
            "body": [e for e in state if e[0] in ("bodyPos", "bodyFwd", "bodyMs", "bodyLimits", "pathCm",
                                                    "posed", "journey")],
        }
        result = {name: bytes_of(entries, self.N) for name, entries in groups.items()}
        result["state_total"] = bytes_of(state, self.N)
        result["update_scratch"] = bytes_of(self.layout["scratch"], self.N)
        result["eye_slots_and_pose"] = bytes_of(self.layout["eye"], self.N)
        result["hd_input_float32"] = 4 * 10 * OUTPUTS
        return result

    def update(self, lanes=32):
        if self.backend.kind == "cuda":
            self.backend.ext.update(self.address, lanes)
        else:
            self.backend.lib.gm_update(self.address)

    def read(self, agents, near, far):
        """Write readouts of ``agents`` (int32) to CHW float32 near/far batches."""
        if self.backend.kind == "cuda":
            self.backend.ext.read(self.address, agents, near, far)
        else:
            self.backend.lib.gm_read(self.address, agents.ctypes.data, len(agents), near.ctypes.data, far.ctypes.data)

    def scatter(self, table: "EyeTable", updates):
        if self.backend.kind == "cuda":
            self.backend.ext.scatter(self.address, table.address, updates)
        else:
            self.backend.lib.gm_scatter(self.address, table.address, updates.ctypes.data)

    def load_slots(self, rays_per_agent):
        """Initial eye output (RAY [N, R]) for agents that start mid-capture."""
        rays = rays_per_agent.reshape(self.N * self.R)
        for name, value in (("rayOrigin", rays["origin"]), ("rayPoint", rays["point"]),
                            ("rayDistance", rays["distance"]), ("rayNormalY", rays["normal_y"]),
                            ("rayKind", rays["kind"]), ("rayHit", rays["hit"]),
                            ("rayTime", times32(rays["time"]))):
            self.backend.store(self.buffers[name][0], np.ascontiguousarray(value).astype(self.buffers[name][1]))


def times32(values):
    values = np.asarray(values, np.uint64)
    if values.size and values.max() >= 2 ** 32 - 1:
        raise ValueError("game time does not fit the 32-bit state")
    return values.astype(np.uint32)


class EyeTable:
    """Recorded updates of several captures, concatenated."""

    def __init__(self, backend: Backend, records):
        self.first = np.cumsum([0] + [c.updates for c in records]).astype(np.int64)
        poses = np.concatenate([c.poses for c in records])
        changed = np.concatenate([c.changed for c in records])
        offsets = np.cumsum([0] + [len(c.slot) for c in records])
        begin = np.concatenate([c.begin[:-1] + o for c, o in zip(records, offsets)] + [[offsets[-1]]])
        self.views_after = np.concatenate([c.views_after for c in records])
        arrays = dict(begin=begin.astype(np.int64), slot=np.concatenate([c.slot for c in records]),
                      origin=changed["origin"].astype(np.float32), point=changed["point"].astype(np.float32),
                      distance=changed["distance"], normalY=changed["normal_y"], kind=changed["kind"],
                      hit=changed["hit"], time=times32(changed["time"]),
                      pos=poses["position"].astype(np.float32), fwd=poses["forward"].astype(np.float32),
                      ms=times32(poses["ms"]),
                      limits=np.stack((poses["step"], poses["slope"], poses["body"]), 1).astype(np.float32))
        self.buffers = {}
        self.table = Table(updates=len(poses), changed=len(changed))
        for name, value in arrays.items():
            buffer, address = backend.upload(value)
            self.buffers[name] = buffer
            setattr(self.table, name, address)
        self.address = ctypes.addressof(self.table)
        self.device_bytes = sum(np.asarray(v).nbytes for v in arrays.values())
