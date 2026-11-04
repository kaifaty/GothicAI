#!/usr/bin/env python3
"""Build the stand-in quiver (a plain tube) and register it as ITRW_QUIVER in a content root.

    python3 scripts/content/mesh3d/quiver_stub.py --output ~/.local/share/gothic-ai/content

The engine hangs ITRW_QUIVER on the right hip of a bowman (OpenGothic/common/graphics/mesh/quiver.h,
reports/archery/2026-11-04-quiver-mount). Its frame: the centre of the mouth at the origin, the quiver
standing along -Y (glTF metres; the loader mirrors X). The real model replaces meshes/quiver-stub/model.glb
or the ITRW_QUIVER entry; no engine change. Sizes as in quiver.h: 68 cm long, 9.5 cm across the mouth,
8 cm at the bottom, a 4 mm wall. The material is a Gothic leather texture.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct

LENGTH = 0.68
MOUTH_R = 0.0475
BOTTOM_R = 0.04
WALL = 0.004
SEGMENTS = 24
MATERIAL = "MODILEATHERSLEEVE01"
KEY = "ITRW_QUIVER"
FOLDER = "quiver-stub"


def tube() -> tuple[list[float], list[float], list[float], list[int]]:
    pos: list[float] = []
    nrm: list[float] = []
    uv: list[float] = []
    idx: list[int] = []

    def vertex(p, n, t) -> int:
        pos.extend(p)
        nrm.extend(n)
        uv.extend(t)
        return len(pos) // 3 - 1

    def ring_strip(r0, y0, r1, y1, inward, v0, v1):
        slope = (r0 - r1) / (y0 - y1) if y0 != y1 else 0.0
        top, bottom = [], []
        for i in range(SEGMENTS + 1):
            a = 2 * math.pi * i / SEGMENTS
            c, s = math.cos(a), math.sin(a)
            n = [c, -slope, s]
            ln = math.sqrt(sum(x * x for x in n))
            n = [x / ln * (-1 if inward else 1) for x in n]
            u = i / SEGMENTS
            top.append(vertex([r0 * c, y0, r0 * s], n, [u, v0]))
            bottom.append(vertex([r1 * c, y1, r1 * s], n, [u, v1]))
        for i in range(SEGMENTS):
            a, b, c2, d = top[i], top[i + 1], bottom[i], bottom[i + 1]
            # outward faces counter-clockwise seen from outside
            quad = [a, d, b, a, c2, d] if inward else [a, b, d, a, d, c2]
            idx.extend(quad)

    def disc(r, y, up):
        n = [0.0, 1.0 if up else -1.0, 0.0]
        centre = vertex([0.0, y, 0.0], n, [0.5, 0.5])
        rim = []
        for i in range(SEGMENTS + 1):
            a = 2 * math.pi * i / SEGMENTS
            rim.append(vertex([r * math.cos(a), y, r * math.sin(a)], n,
                              [0.5 + 0.5 * math.cos(a), 0.5 + 0.5 * math.sin(a)]))
        for i in range(SEGMENTS):
            idx.extend([centre, rim[i + 1], rim[i]] if up else [centre, rim[i], rim[i + 1]])

    def annulus(r0, r1, y):
        n = [0.0, 1.0, 0.0]
        inner, outer = [], []
        for i in range(SEGMENTS + 1):
            a = 2 * math.pi * i / SEGMENTS
            c, s = math.cos(a), math.sin(a)
            inner.append(vertex([r0 * c, y, r0 * s], n, [i / SEGMENTS, 0.0]))
            outer.append(vertex([r1 * c, y, r1 * s], n, [i / SEGMENTS, 0.02]))
        for i in range(SEGMENTS):
            idx.extend([inner[i], inner[i + 1], outer[i + 1], inner[i], outer[i + 1], outer[i]])

    ring_strip(MOUTH_R, 0.0, BOTTOM_R, -LENGTH, False, 0.0, 2.0)
    ring_strip(MOUTH_R - WALL, 0.0, BOTTOM_R - WALL, -LENGTH + WALL, True, 0.0, 2.0)
    annulus(MOUTH_R - WALL, MOUTH_R, 0.0)
    disc(BOTTOM_R, -LENGTH, False)
    disc(BOTTOM_R - WALL, -LENGTH + WALL, True)
    return pos, nrm, uv, idx


def glb(pos: list[float], nrm: list[float], uv: list[float], idx: list[int]) -> bytes:
    count = len(pos) // 3
    blobs = [struct.pack(f"<{len(pos)}f", *pos), struct.pack(f"<{len(nrm)}f", *nrm),
             struct.pack(f"<{len(uv)}f", *uv), struct.pack(f"<{len(idx)}H", *idx)]
    views, offset, binary = [], 0, b""
    for blob in blobs:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(blob)})
        blob += b"\0" * (-len(blob) % 4)
        binary += blob
        offset += len(blob)
    xs, ys, zs = pos[0::3], pos[1::3], pos[2::3]
    doc = {
        "asset": {"version": "2.0", "generator": "quiver_stub.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": KEY, "mesh": 0}],
        "materials": [{"name": MATERIAL}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                    "indices": 3, "material": 0}]}],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": views,
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3",
             "min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]},
            {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": count, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
        ],
    }
    text = json.dumps(doc, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    chunks = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(binary), 0x004E4942) + binary
    return struct.pack("<III", 0x46546C67, 2, 12 + len(chunks)) + chunks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, required=True, help="content root (OPENGOTHIC_CONTENT)")
    args = parser.parse_args()
    meshes = args.output.expanduser() / "meshes"
    (meshes / FOLDER).mkdir(parents=True, exist_ok=True)
    (meshes / FOLDER / "model.glb").write_bytes(glb(*tube()))
    table_path = meshes / "meshes.json"
    table = json.loads(table_path.read_text()) if table_path.exists() else {}
    table[KEY] = {"glb": f"{FOLDER}/model.glb"}
    table_path.write_text(json.dumps(table, indent=2, ensure_ascii=False) + "\n")
    print(f"{KEY} -> {meshes / FOLDER / 'model.glb'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
