#!/usr/bin/env python3
"""Compare two 1 m canopy bakes with alpha-tested fir L4 rays, without the game.

Requires NumPy. Pass the `content` fixture from gi-crown-5-20261125. The
reference traces foliage only. Ground heights and normals are receiver tests.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np


ASSETS = {
    "meshes/trees_fir/OWN_TREE_FIR_A_4_L4.glb": "12e49adf8e0a46ac5ae1e35713b38e756d93877cecf86704ae8d06bdbba5dfea",
    "materials/trees_fir/boughs-albedo.dds": "c2660be4cb430cb32265dba4cdaf1e653773a86d6506d565f2fbfcbfde2bd8e2",
    "worlds/NEWWORLD/vobs.json": "26f79531c4d3a7cb67d9441b25318cf8bfb65beb3a2cc8eef0e623c9e6f8f7f5",
}


def fixture(content):
    for path, expected in ASSETS.items():
        data = (content / path).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"Fixture hash mismatch: {path}")
        yield data


def foliage(glb):
    size, kind = struct.unpack_from("<II", glb, 12)
    if kind != 0x4E4F534A:
        raise ValueError("Expected a GLB JSON chunk")
    model = json.loads(glb[20:20 + size])

    def accessor(index):
        entry = model["accessors"][index]
        view = model["bufferViews"][entry["bufferView"]]
        dtype = np.dtype({5123: "<u2", 5125: "<u4", 5126: "<f4"}[entry["componentType"]])
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[entry["type"]]
        start = 28 + size + view.get("byteOffset", 0) + entry.get("byteOffset", 0)
        stride = view.get("byteStride", width * dtype.itemsize)
        return np.ndarray((entry["count"], width), dtype, glb, start, (stride, dtype.itemsize))

    mesh, = model["meshes"]
    primitive, = [p for p in mesh["primitives"] if model["materials"][p["material"]].get("alphaMode") == "MASK"]
    indices = accessor(primitive["indices"]).reshape(-1, 3)
    xyz = accessor(primitive["attributes"]["POSITION"])[indices] * (-1, 1, 1)
    uv = accessor(primitive["attributes"]["TEXCOORD_0"])[indices]
    return xyz, uv


def alpha_image(dds):
    height, width = struct.unpack_from("<II", dds, 12)
    if dds[84:88] != b"DXT5" or width % 4 or height % 4:
        raise ValueError("Expected a block-aligned BC3 texture")
    blocks = np.frombuffer(dds, np.uint8, (width // 4) * (height // 4) * 16, 128).reshape(-1, 16)
    a, b = blocks[:, 0].astype(np.uint16), blocks[:, 1].astype(np.uint16)
    table = [a, b]
    for i in range(1, 7):
        short = ((5 - i) * a + i * b) // 5 if i < 5 else (0 if i == 5 else 255)
        table.append(np.where(a > b, ((7 - i) * a + i * b) // 7, short))
    codes = blocks[:, 2:8].astype(np.uint64) @ (1 << np.arange(0, 48, 8, dtype=np.uint64))
    index = ((codes[:, None] >> np.arange(0, 48, 3, dtype=np.uint64)) & 7).astype(np.intp)
    alpha = np.take_along_axis(np.stack(table, axis=1), index, axis=1)
    return alpha.reshape(height // 4, width // 4, 4, 4).transpose(0, 2, 1, 3).reshape(height, width)


def opaque(uv, image):
    height, width = image.shape
    x, y = (uv * (width, height) - 0.5).T
    i, j = np.floor(x).astype(int), np.floor(y).astype(int)
    tx, ty = x - i, y - j
    lower = (1 - tx) * image[j % height, i % width] + tx * image[j % height, (i + 1) % width]
    upper = (1 - tx) * image[(j + 1) % height, i % width] + tx * image[(j + 1) % height, (i + 1) % width]
    return (1 - ty) * lower + ty * upper > 127.5


def bake(xyz, uv, alpha, transforms, positions):
    count = 64
    r = np.sqrt((np.arange(count) + 0.5) / count)
    s = (np.arange(count) / ((1 + math.sqrt(5)) / 2)) % 1
    barycentric = np.stack((1 - r, r * (1 - s), r * s), axis=1)
    covered = opaque((barycentric @ uv).reshape(-1, 2), alpha).reshape(-1, count)
    samples = (barycentric @ xyz)[covered]
    areas = np.linalg.norm(np.cross(xyz[:, 1] - xyz[:, 0], xyz[:, 2] - xyz[:, 0]), axis=1) / 2
    # Mean projected area of a two-sided patch is its area times 1/2.
    weights = np.broadcast_to(areas[:, None] / (2 * count), covered.shape)[covered]
    world = xyz.reshape(-1, 3) @ transforms.transpose(0, 2, 1) + positions[:, None, :]
    low = np.floor(world.min(axis=(0, 1))).astype(int)
    high = np.ceil(world.max(axis=(0, 1))).astype(int)
    layered = np.zeros(high - low, np.float32)
    for transform, position in zip(transforms, positions):
        cells = np.floor(samples @ transform.T + position).astype(int) - low
        scale = np.linalg.norm(transform[:, 1])
        np.add.at(layered, tuple(cells.T), weights * scale ** 2)
    occupied = layered > 0
    has_mass = occupied.any(axis=1)
    bottom = occupied.argmax(axis=1)
    top = occupied.shape[1] - occupied[:, ::-1, :].argmax(axis=1)
    y = np.arange(occupied.shape[1])[None, :, None]
    slab = (y >= bottom[:, None, :]) & (y < top[:, None, :]) & has_mass[:, None, :]
    single = slab * (layered.sum(axis=1) / np.maximum(1, top - bottom))[:, None, :]
    return low, world.reshape(-1, len(xyz), 3, 3), np.stack((single, layered), axis=-1)


def box_interval(origins, directions, low, high):
    ends = (np.stack((low, high))[:, None, :] - origins) / directions
    return ends.min(axis=0).max(axis=1), ends.max(axis=0).min(axis=1)


def reference(origins, directions, near, far, world, uv, alpha):
    hits = np.zeros(len(origins), bool)
    for triangles in world:
        start, end = box_interval(origins, directions, triangles.min(axis=(0, 1)), triangles.max(axis=(0, 1)))
        active = np.flatnonzero((np.maximum(start, near) <= np.minimum(end, far)) & ~hits)
        if not len(active):
            continue
        a, b, c = triangles.transpose(1, 0, 2)
        ab, ac = b - a, c - a
        p = np.cross(directions[active, None, :], ac)
        det = (p * ab).sum(axis=2)
        safe = np.where(abs(det) > 1e-10, det, 1.0)
        s = origins[active, None, :] - a
        u = (s * p).sum(axis=2) / safe
        q = np.cross(s, ab)
        v = (directions[active, None, :] * q).sum(axis=2) / safe
        t = (q * ac).sum(axis=2) / safe
        ray, face = np.where((abs(det) > 1e-10) & (u >= 0) & (v >= 0) & (u + v <= 1) & (t >= near) & (t <= far))
        base, edge_a, edge_b = uv[face].transpose(1, 0, 2)
        at = base + u[ray, face, None] * (edge_a - base) + v[ray, face, None] * (edge_b - base)
        hits[active[ray[opaque(at, alpha)]]] = True
    return ~hits


def attenuation(origins, directions, near, far, low, density):
    """Exact grid-boundary traversal through the piecewise constant bakes."""
    high = low + density.shape[:3]
    enter, leave = box_interval(origins, directions, low, high)
    end = np.minimum(leave, far)
    t = np.maximum(enter, near)
    optical_depth = np.zeros((len(origins), density.shape[-1]))
    active = np.flatnonzero(t + 1e-9 < end)
    cells = np.zeros_like(origins, dtype=int)
    start = origins[active] + t[active, None] * directions[active]
    cells[active] = np.clip(np.floor(np.nextafter(start, start + directions[active])).astype(int), low, high - 1)
    step = np.sign(directions).astype(int)
    while len(active):
        cell = cells[active]
        face = cell + (step[active] > 0)
        boundary = (face - origins[active]) / directions[active]
        exit_t = boundary.min(axis=1)
        next_t = np.clip(exit_t, t[active], end[active])
        optical_depth[active] += density[tuple((cell - low).T)] * (next_t - t[active])[:, None]
        cells[active] += step[active] * (boundary == exit_t[:, None])
        t[active] = next_t
        inside = ((cells[active] >= low) & (cells[active] < high)).all(axis=1)
        active = active[inside & (next_t + 1e-9 < end[active])]
    return np.exp(-optical_depth)


def sunlight(hour, minute):
    """Sky::updateLight for the two archived daytime controls."""
    clock = hour * 60 + minute
    pulse = (clock - (4 * 60 + 45)) / ((13 * 60 + 9) - (4 * 60 + 45))
    azimuth = math.tau * (1 - (clock / 1440 + 0.25) % 1)
    vec = np.array((-math.sin(azimuth) * 0.56, pulse, math.cos(azimuth) * 0.56))
    return vec / np.linalg.norm(vec)


def measure(xyz, uv, alpha, placements):
    positions = np.array([item["position"] for item in placements]) / 100
    transforms = np.array([item["rotation"] for item in placements])
    low, world, density = bake(xyz, uv, alpha, transforms, positions)
    rng = np.random.default_rng(73013)
    # Surface patches near the camera, a gap between stems, the edge, and an inclined plane.
    regions = {"inside": (541.4, 16.50, -107.8, 0), "gap": (542.5, 16.50, -107.9, 0),
               "edge": (516.9, 14.10, -103.0, 0), "slope": (531.0, 28.50, -145.0, 0.45)}
    blocks = np.stack(np.meshgrid(np.arange(4), np.arange(4)), axis=-1).reshape(-1, 2)
    result = {}
    for name, (x, y, z, slope) in regions.items():
        points = (np.repeat(blocks, 4, axis=0) + rng.random((64, 2))) / 2 - 1
        origins = np.column_stack((x + points[:, 0], y + slope * points[:, 0], z + points[:, 1]))
        # Cosine-weighted sky samples so their mean is irradiance/open irradiance for a uniform sky.
        radii = np.sqrt((np.arange(256) + 0.5) / 256)
        phase = math.tau * ((np.arange(256) / ((1 + math.sqrt(5)) / 2) + 0.137) % 1)
        sky = np.column_stack((radii * np.cos(phase), np.sqrt(1 - radii ** 2), radii * np.sin(phase)))
        rotation = np.array([[1, -slope, 0], [slope, 1, 0], [0, 0, math.sqrt(1 + slope ** 2)]]) / math.sqrt(1 + slope ** 2)
        sky = sky @ rotation.T
        for label, dirs, near, far in (("sun_12_03", sunlight(12, 3)[None], 5, 500),
                                       ("sun_05_30", sunlight(5, 30)[None], 5, 500),
                                       ("sky", sky, 0.01, 200)):
            origins_by_ray = np.repeat(origins, len(dirs), axis=0)
            directions = np.tile(dirs, (len(origins), 1))
            exact = reference(origins_by_ray, directions, near, far, world, uv, alpha).reshape(-1, len(dirs)).mean(axis=1)
            approximate = attenuation(origins_by_ray, directions, near, far, low, density).reshape(-1, len(dirs), 2).mean(axis=1)
            error = approximate - exact[:, None]
            block_error = error.reshape(-1, 4, 2).mean(axis=1)
            dark = exact.reshape(-1, 4).mean(axis=1) <= 0.05
            result[f"{name}_{label}"] = {
                "rays": len(origins_by_ray), "open_pct": 100 * exact.mean(),
                "bias_pp": (100 * error.mean(axis=0)).tolist(),
                "p95_abs_block_pp": (100 * np.quantile(abs(block_error), 0.95, axis=0)).tolist(),
                "dark_blocks": int(dark.sum()),
                "dark_p95_abs_block_pp": (100 * np.quantile(abs(block_error[dark]), 0.95, axis=0)).tolist() if dark.any() else None,
                "bright_dark_crossings": (((approximate < 0.25) & (exact[:, None] > 0.75)) |
                                           ((approximate > 0.75) & (exact[:, None] < 0.25))).sum(axis=0).tolist(),
            }
    return {"spruces": len(placements), "triangles_per_spruce": len(xyz), "variants": ["slab", "layered"], "regions": result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("content", type=Path)
    args = parser.parse_args()
    glb, dds, vobs = fixture(args.content)
    xyz, uv = foliage(glb)
    trees = [item for item in json.loads(vobs)["vobs"] if item["vob_name"].startswith("tree-cost-")]
    print(json.dumps(measure(xyz, uv, alpha_image(dds), trees), indent=2))


if __name__ == "__main__":
    main()
