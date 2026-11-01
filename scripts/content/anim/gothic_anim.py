"""Read and write skeletons and animations of the converted base (models/*.MDH, *.MAN).

Pure Python + numpy, so the same module runs in Blender and outside it.

Conventions of the base (see OpenGothic/common/modeldata.cpp, graphics/mesh/animmath.cpp):
- 4x4 matrices are stored column-major: translation is elements 12..14.
- A MAN sample is (position, quaternion w x y z) of a node relative to its parent.
  The quaternion is the conjugate of the usual column-vector rotation: the engine writes
  its matrix transposed. `sample_matrix` returns the usual matrix.
- Units are centimetres, Y is up, the human faces +Z.
"""
from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np


@dataclass
class Node:
    name: str
    parent: int
    rest: np.ndarray  # 4x4 local bind transform, usual column-vector convention


@dataclass
class Skeleton:
    nodes: list[Node]
    root_translation: np.ndarray

    def index(self, name: str) -> int:
        for i, n in enumerate(self.nodes):
            if n.name == name:
                return i
        raise KeyError(name)


@dataclass
class Animation:
    name: str
    next: str
    layer: int
    fps: float
    frame_count: int
    node_indices: list[int]
    positions: np.ndarray  # [frames, nodes, 3] cm, parent space
    rotations: np.ndarray  # [frames, nodes, 4] usual quaternion w x y z
    events: list = field(default_factory=list)

    @property
    def duration(self) -> float:
        return self.frame_count / self.fps


def _read(path: Path) -> tuple[dict, bytes]:
    meta = json.loads(path.read_text())
    buf = b""
    if meta.get("byte_length"):
        buf = (path.parent / meta["buffer"]).read_bytes()
    return meta, buf


def _matrix(values) -> np.ndarray:
    # Column-major storage → row-major numpy matrix.
    return np.array(values, dtype=np.float64).reshape(4, 4).T


def read_skeleton(path: Path) -> Skeleton:
    meta, _ = _read(Path(path))
    data = meta["data"]
    nodes = [Node(n["name"], n["parent_index"], _matrix(n["transform"])) for n in data["nodes"]]
    root_tr = np.array(data["root_translation"], dtype=np.float64)
    for n in nodes:
        if n.parent < 0:  # the engine translates root nodes by root_translation (skeleton.cpp)
            t = np.eye(4)
            t[:3, 3] = root_tr
            n.rest = n.rest @ t
    return Skeleton(nodes, root_tr)


@dataclass
class SkinMesh:
    positions: np.ndarray        # [V, 3] bind pose, model space, cm
    triangles: np.ndarray        # [T, 3] position indices
    weights: list[list[tuple[int, float]]]  # per vertex: (node index, weight)


def read_skin_mesh(path: Path, skel: Skeleton) -> SkinMesh:
    """First soft-skin mesh of an MDM record, posed in the skeleton's bind pose."""
    meta, buf = _read(Path(path))
    m = meta["data"]["meshes"][0]
    mesh = m["mesh"]
    bind = world_pose(skel, None, 0)
    w = m["weights"]
    at, count = w["offset"], w["count"]
    pos = np.zeros((count, 3))
    weights = []
    for v in range(count):
        (n,) = struct.unpack_from("<I", buf, at)
        at += 4
        vw = []
        for _ in range(n):
            weight, x, y, z = struct.unpack_from("<4f", buf, at)
            node = buf[at + 16]
            at += 17
            pos[v] += weight * (bind[node] @ np.array([x, y, z, 1.0]))[:3]
            vw.append((node, weight))
        weights.append(vw)
    tris = []
    for sub in mesh["sub_meshes"]:
        wd = sub["wedges"]
        wedge_pos = [struct.unpack_from("<H", buf, wd["offset"] + i * 22 + 20)[0] for i in range(wd["count"])]
        tr = sub["triangles"]
        for i in range(tr["count"]):
            a, b, c = struct.unpack_from("<3H", buf, tr["offset"] + i * 6)
            tris.append((wedge_pos[a], wedge_pos[b], wedge_pos[c]))
    return SkinMesh(pos, np.array(tris, dtype=np.int64), weights)


def _quantized_rotation(codes: np.ndarray) -> np.ndarray:
    scale = (1.0 / 65535.0) * 2.1
    xyz = (codes.astype(np.float64) - 32767.0) * scale
    len_q = (xyz * xyz).sum(-1)
    w = np.sqrt(np.clip(1.0 - len_q, 0.0, None))
    over = len_q > 1.0
    xyz[over] /= np.sqrt(len_q[over])[:, None]
    w[over] = 0.0
    return np.concatenate([w[:, None], xyz], axis=-1)


def read_animation(path: Path) -> Animation:
    meta, buf = _read(Path(path))
    d = meta["data"]
    frames, nodes = d["frame_count"], d["node_count"]
    s = d["samples"]
    count = s["count"]
    if s["encoding"] == "quantized16":
        raw = np.frombuffer(buf, dtype="<u2", count=count * 6, offset=s["offset"]).reshape(count, 6)
        pos = raw[:, :3].astype(np.float64) * d["sample_position_scale"] + d["sample_position_min"]
        rot = _quantized_rotation(raw[:, 3:])
    elif s["encoding"] == "float32":
        raw = np.frombuffer(buf, dtype="<f4", count=count * 7, offset=s["offset"]).reshape(count, 7)
        pos = raw[:, :3].astype(np.float64)
        rot = raw[:, 3:].astype(np.float64)
    else:
        raise ValueError(f"unsupported encoding {s['encoding']}")
    # Stored quaternion is the conjugate of the usual one.
    rot = rot * np.array([1.0, -1.0, -1.0, -1.0])
    ni = d["node_indices"]
    idx = list(np.frombuffer(buf, dtype="<u4", count=ni["count"], offset=ni["offset"]))
    return Animation(
        name=d["name"], next=d.get("next", ""), layer=d["layer"], fps=d["fps"],
        frame_count=frames, node_indices=[int(i) for i in idx],
        positions=pos.reshape(frames, nodes, 3), rotations=rot.reshape(frames, nodes, 4),
        events=d.get("events", []))


def quat_matrix(q: np.ndarray) -> np.ndarray:
    w, x, y, z = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
    ])


def sample_matrix(position: np.ndarray, rotation: np.ndarray) -> np.ndarray:
    m = np.eye(4)
    m[:3, :3] = quat_matrix(rotation)
    m[:3, 3] = position
    return m


def world_pose(skel: Skeleton, anim: Animation | None, frame: int) -> np.ndarray:
    """Model-space 4x4 transforms of every node at a frame (bind pose for nodes the clip lacks)."""
    local = [n.rest.copy() for n in skel.nodes]
    if anim is not None:
        for col, node in enumerate(anim.node_indices):
            if node < len(local):
                local[node] = sample_matrix(anim.positions[frame, col], anim.rotations[frame, col])
    out = np.zeros((len(skel.nodes), 4, 4))
    for i, n in enumerate(skel.nodes):
        out[i] = local[i] if n.parent < 0 else out[n.parent] @ local[i]
    return out


def write_animation(path: Path, anim: Animation, source: str) -> None:
    """Write a float32 MAN record next to its .bin, in the base's JSON v1 layout."""
    path = Path(path)
    frames, nodes = anim.frame_count, len(anim.node_indices)
    rot = anim.rotations * np.array([1.0, -1.0, -1.0, -1.0])  # back to the stored conjugate
    samples = np.concatenate([anim.positions, rot], axis=-1).reshape(frames * nodes, 7).astype("<f4")
    blob = samples.tobytes()
    idx_offset = len(blob)
    blob += np.array(anim.node_indices, dtype="<u4").tobytes()
    pos = anim.positions.reshape(-1, 3)
    bbox = [pos.min(0).tolist(), pos.max(0).tolist()]
    bin_name = path.name[:-len(".json")] + ".bin" if path.name.endswith(".json") else path.name + ".bin"
    meta = {
        "format": "gothic-model", "version": 1, "kind": "animation",
        "buffer": bin_name, "byte_length": len(blob),
        "data": {
            "name": anim.name, "next": anim.next, "layer": anim.layer,
            "frame_count": frames, "node_count": nodes, "fps": anim.fps, "fps_source": anim.fps,
            "sample_position_min": 0.0, "sample_position_scale": 1.0, "bbox": bbox,
            "checksum": 0, "source_path": source,
            "source_date": {k: 0 for k in ("year", "month", "day", "hour", "minute", "second")},
            "source_script": "",
            "samples": {"offset": 0, "count": frames * nodes, "encoding": "float32"},
            "events": anim.events,
            "node_indices": {"offset": idx_offset, "count": nodes},
        },
    }
    (path.parent / bin_name).write_bytes(blob)
    path.write_text(json.dumps(meta, ensure_ascii=False, indent=1))


def root_travel(anim: Animation) -> np.ndarray:
    """Root (node 0) offset of every frame from frame 0, as the engine's moveTr sees it."""
    if anim.node_indices[0] != 0:
        return np.zeros((anim.frame_count, 3))
    return anim.positions[:, 0] - anim.positions[0, 0]


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="Print a MAN record summary")
    ap.add_argument("man", type=Path)
    a = read_animation(ap.parse_args().man)
    tr = root_travel(a)
    print(f"{a.name}: {a.frame_count} frames @ {a.fps:g} fps = {a.duration*1000:.0f} ms, "
          f"{len(a.node_indices)} nodes, next {a.next or '-'}")
    print("root travel (cm):", np.round(tr[-1], 1), " min y", round(float(tr[:, 1].min()), 1))
    print("root z per frame:", np.round(tr[:, 2]).astype(int).tolist())
