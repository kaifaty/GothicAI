#!/usr/bin/env python3
"""How much texture memory the world mesh needs when the mip is chosen by the size on screen.

    census.py <ready base> [--world NEWWORLD] [--height 1440] [--fov 67.5]

Reads the world chunks of a ready base (worlds/<W>/chunks/*.glb) and the DDS headers of its textures.
For three camera places it prints the bytes of world-mesh textures under several ways to choose the
resident mip, for the textures as they are and enlarged x2, x4, x8 per side:

  full     every texture of the loaded range with the whole chain;
  rings    today's rule: whole chain within 2 chunks of the camera, otherwise without the top 2 mips;
  cell N   one demand per material and N-metre cell: distance to the cell's box, the cell's lowest texel
           density (its most stretched triangle decides, as it shows the largest texels);
  oracle   every triangle on its own: distance to the triangle, its own texel density.

The density of a triangle is that of its sparser texture axis (`--density axis`, the default): with
anisotropic filtering the GPU picks the mip by the shorter axis of the pixel's footprint in the texture.
`--density area` is the square root of the area ratio, which is too coarse on a stretched UV map.
`--quantise` rounds the cell demands as the engine registers them (a power of two, towards the sharper side).

The needed mip of a surface with D texels per metre at distance d is floor(log2(D / ppm(d))), where
ppm(d) = height / (2 d tan(fov/2)) is pixels per metre for a surface facing the camera; `--bias` adds
whole mips of reserve. Nothing lower than a 64-pixel side is dropped, as in today's tail.
"""
from __future__ import annotations

import argparse
import json
import math
import struct
from collections import defaultdict
from pathlib import Path

import numpy as np

CHUNK = 64.0
FAR_RING = 16
FORMATS = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16}


def read_glb(path: Path):
    data = path.read_bytes()
    size = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + size])
    binary = memoryview(data)[20 + size + 8:]

    def accessor(index, dtype, width):
        acc = doc["accessors"][index]
        view = doc["bufferViews"][acc["bufferView"]]
        start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        return np.frombuffer(binary, dtype=dtype, count=acc["count"] * width, offset=start).reshape(acc["count"], width)

    for prim in doc["meshes"][0]["primitives"]:
        material = doc["materials"][prim["material"]]["extras"]["gothic"]
        idx_acc = doc["accessors"][prim["indices"]]
        idx_type = {5123: np.uint16, 5125: np.uint32}[idx_acc["componentType"]]
        idx = accessor(prim["indices"], idx_type, 1).reshape(-1, 3).astype(np.int64)
        pos = accessor(prim["attributes"]["POSITION"], np.float32, 3).astype(np.float64) * 0.01
        pos[:, 0] *= -1.0   # the chunk files store x mirrored against the game's frame (waypoints, the camera)
        uv = accessor(prim["attributes"]["TEXCOORD_0"], np.float32, 2).astype(np.float64)
        yield material.get("texture", ""), pos[idx], uv[idx]


def texture_sizes(base: Path) -> dict:
    index = json.loads((base / "textures/textures.json").read_text())["textures"]
    out = {}
    for key, name in index.items():
        with (base / "textures" / name).open("rb") as file:
            header = file.read(128)
        if header[:4] != b"DDS " or header[84:88] not in FORMATS:
            continue
        height, width = struct.unpack_from("<II", header, 12)
        out[key.upper()] = (width, height, FORMATS[header[84:88]], max(1, struct.unpack_from("<I", header, 28)[0]))
    return out


def chain_bytes(width, height, block, first):
    """Bytes of the chain from level `first` down to 1x1."""
    total = 0
    level = 0
    while True:
        if level >= first:
            total += max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * block
        if width == 1 and height == 1:
            return total
        width, height, level = max(1, width // 2), max(1, height // 2), level + 1


def floor_level(width, height):
    """The lowest level that may be the top of a resident chain: both sides stay at least 64."""
    level = 0
    while min(width, height) >> (level + 1) >= 64:
        level += 1
    return level


def point_triangle_distance(p, tri):
    a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]

    def segment(u, v):
        d = v - u
        t = np.clip(((p - u) * d).sum(1) / np.maximum((d * d).sum(1), 1e-12), 0, 1)
        return np.linalg.norm(u + d * t[:, None] - p, axis=1)

    best = np.minimum(np.minimum(segment(a, b), segment(b, c)), segment(c, a))
    n = np.cross(b - a, c - a)
    length = np.linalg.norm(n, axis=1)
    ok = length > 1e-12
    n = n / np.maximum(length, 1e-12)[:, None]
    height = ((p - a) * n).sum(1)
    q = p - n * height[:, None]
    inside = ok
    for u, v in ((a, b), (b, c), (c, a)):
        inside &= (np.cross(v - u, q - u) * n).sum(1) >= 0
    return np.where(inside, np.minimum(best, np.abs(height)), best)


def uv_density(tri, uv, axis):
    """World area of every triangle and its UV units per metre: sqrt(UV area / area), or with `axis` the
    smaller singular value of the UV-per-metre map (the sparser of the two texture axes)."""
    e1, e2 = tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]
    area = 0.5 * np.linalg.norm(np.cross(e1, e2), axis=1)
    u1, u2 = uv[:, 1] - uv[:, 0], uv[:, 2] - uv[:, 0]
    det_u = u1[:, 0] * u2[:, 1] - u1[:, 1] * u2[:, 0]
    good = (area > 1e-4) & (np.abs(det_u) > 0)
    mean = np.sqrt(0.5 * np.abs(det_u) / np.maximum(area, 1e-12))
    if not axis:
        return area, mean, good
    # the triangle's plane: P maps plane coordinates to the two edges, J = U P^-1 is UV per metre
    l1 = np.maximum(np.linalg.norm(e1, axis=1), 1e-12)
    t1 = e1 / l1[:, None]
    a = (e2 * t1).sum(1)
    b = np.maximum(np.linalg.norm(e2 - t1 * a[:, None], axis=1), 1e-12)
    j11, j12 = u1[:, 0] / l1, (u2[:, 0] - u1[:, 0] * a / l1) / b
    j21, j22 = u1[:, 1] / l1, (u2[:, 1] - u1[:, 1] * a / l1) / b
    trace = j11 * j11 + j12 * j12 + j21 * j21 + j22 * j22
    det = np.abs(j11 * j22 - j12 * j21)
    low = np.sqrt(np.maximum(0.5 * (trace - np.sqrt(np.maximum(trace * trace - 4 * det * det, 0))), 0))
    return area, low, good & (low > 0)


def box_distance(p, lo, hi):
    return np.linalg.norm(np.maximum(np.maximum(lo - p, p - hi), 0), axis=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("base", type=Path)
    parser.add_argument("--world", default="NEWWORLD")
    parser.add_argument("--height", type=float, default=1440)
    parser.add_argument("--fov", type=float, default=67.5)
    parser.add_argument("--bias", type=int, default=0, help="whole mips of reserve (sharper than needed)")
    parser.add_argument("--density", choices=("area", "axis"), default="axis",
                        help="UV density of a triangle: sqrt of the area ratio, or its sparser texture axis (default)")
    parser.add_argument("--quantise", action="store_true",
                        help="cell demands as the engine registers them: a power of two, rounded to the sharper side")
    args = parser.parse_args()

    sizes = texture_sizes(args.base)
    waynet = json.loads((args.base / "worlds" / args.world / "waynet.json").read_text())["waynet"]
    points = {}
    for point in waynet["points"]:
        points[point["name"].upper()] = [v * 0.01 for v in point["position"]]
    places = {
        "market": points["NW_CITY_MERCHANT_TRADE_04"],
        "harbour": [53.91, 3.20, -58.22],
        "forest": points["NW_FOREST_PATH_16"],
    }
    for name in places:
        places[name] = np.array(places[name]) + np.array([0, 1.7, 0])

    chunks = {}
    for path in sorted((args.base / "worlds" / args.world / "chunks").glob("*.glb")):
        if path.name.startswith("._"):
            continue
        cx, cz = (int(v) for v in path.stem.split("_"))
        chunks[(cx, cz)] = [(tex.rsplit(".", 1)[0].upper(), tri, uv) for tex, tri, uv in read_glb(path)]

    # Texel density of the world mesh, weighted by area.
    density, weight, stretch = [], [], []
    for prims in chunks.values():
        for tex, tri, uv in prims:
            if tex not in sizes:
                continue
            width, height = sizes[tex][:2]
            area, mean, good = uv_density(tri, uv, False)
            _, low, good_low = uv_density(tri, uv, True)
            good &= good_low
            density.append(mean[good] * math.sqrt(width * height))
            stretch.append(mean[good] / low[good])
            weight.append(area[good])
    density, weight, stretch = np.concatenate(density), np.concatenate(weight), np.concatenate(stretch)
    order = np.argsort(density)
    cumulative = np.cumsum(weight[order]) / weight.sum()
    print("Texel density of the world mesh, texels per metre, by area:")
    print("  " + ", ".join(f"p{q}: {density[order][np.searchsorted(cumulative, q / 100)]:.0f}" for q in (5, 25, 50, 75, 95, 99)))
    print(f"  triangles: {len(density)}, area: {weight.sum() / 1e6:.2f} km2, textures in the index: {len(sizes)}")
    order = np.argsort(stretch)
    cumulative = np.cumsum(weight[order]) / weight.sum()
    print("Stretch of the UV map (mean density / density of the sparser axis), by area:")
    print("  " + ", ".join(f"p{q}: {stretch[order][min(np.searchsorted(cumulative, q / 100), len(order) - 1)]:.2f}"
                           for q in (50, 75, 90, 95, 99)))

    focal = args.height / (2 * math.tan(math.radians(args.fov) / 2))    # pixels per metre at 1 m
    scales = (1, 2, 4, 8)
    cells = (64, 16, 8)
    for place, eye in places.items():
        home = (math.floor(eye[0] / CHUNK), math.floor(eye[2] / CHUNK))
        ring_of, lam = {}, defaultdict(lambda: defaultdict(lambda: math.inf))   # method -> texture -> log2(D/ppm) at scale 1
        for (cx, cz), prims in chunks.items():
            ring = max(abs(cx - home[0]), abs(cz - home[1]))
            if ring > FAR_RING:
                continue
            for tex, tri, uv in prims:
                if tex not in sizes:
                    continue
                ring_of[tex] = min(ring_of.get(tex, 99), ring)
                width, height = sizes[tex][:2]
                area, per_metre, good = uv_density(tri, uv, args.density == "axis")
                if not good.any():
                    continue
                tri, d_tex = tri[good], per_metre[good] * math.sqrt(width * height)
                dist = np.maximum(point_triangle_distance(eye, tri), 0.25)
                lam["oracle"][tex] = min(lam["oracle"][tex], float(np.min(np.log2(d_tex * dist / focal))))
                centre = tri.mean(axis=1)
                for cell in cells:
                    key = np.floor(centre[:, [0, 2]] / cell).astype(np.int64)
                    _, inverse = np.unique(key, axis=0, return_inverse=True)
                    inverse = inverse.reshape(-1)
                    count = inverse.max() + 1
                    lo = np.full((count, 3), np.inf)
                    hi = np.full((count, 3), -np.inf)
                    np.minimum.at(lo, inverse, tri.min(axis=1))
                    np.maximum.at(hi, inverse, tri.max(axis=1))
                    sparse = np.full(count, np.inf)
                    np.minimum.at(sparse, inverse, d_tex)
                    reach = np.maximum(box_distance(eye, lo, hi), 0.25)
                    need = np.log2(sparse * reach / focal)
                    if args.quantise:
                        # the engine: exponent = ceil(log2(1 / (d u))), level = floor(log2(side) - log2(K) - exponent)
                        side = math.log2(math.sqrt(width * height))
                        need = side - math.log2(focal) - np.ceil(-np.log2(sparse * reach) + side)
                    lam[f"cell {cell}"][tex] = min(lam[f"cell {cell}"][tex], float(np.min(need)))

        print(f"\n{place}: {len(ring_of)} world textures within {FAR_RING} chunks, MiB (x1 / x2 / x4 / x8 per side)")
        rows = defaultdict(list)
        tops = defaultdict(list)
        for scale in scales:
            step = int(math.log2(scale))
            total = defaultdict(int)
            top = defaultdict(int)
            for tex, ring in ring_of.items():
                width, height, block, mips = sizes[tex]
                width, height, mips = width * scale, height * scale, mips + step
                lowest = min(floor_level(width, height), mips - 1)
                total["full"] += chain_bytes(width, height, block, 0)
                tail = min(2, lowest) if (mips > 1 and width >= 128 and height >= 128) else 0
                total["rings"] += chain_bytes(width, height, block, 0 if ring <= 2 else tail)
                for method, values in lam.items():
                    need = values[tex]                      # inf: no textured triangle of this texture in range
                    level = lowest if math.isinf(need) else max(0, min(math.floor(need + step) - args.bias, lowest))
                    total[method] += chain_bytes(width, height, block, level)
                    top[method] += level == 0
            for method, value in total.items():
                rows[method].append(value / (1 << 20))
                tops[method].append(top.get(method))
        for method, values in rows.items():
            extra = ""
            if tops[method][0] is not None:
                extra = "   textures at the top level: " + " / ".join(str(v) for v in tops[method])
            print(f"  {method:8s} " + " / ".join(f"{v:7.1f}" for v in values) + extra)


if __name__ == "__main__":
    main()
