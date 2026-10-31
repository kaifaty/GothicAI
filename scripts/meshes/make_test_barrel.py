#!/usr/bin/env python3
"""Write a procedural GLB barrel and DDS material into new or empty test content.

Usage: python3 scripts/meshes/make_test_barrel.py <directory> [--radius-scale K]
Only the Python standard library is used; no original geometry or pixels.
"""

import argparse
import json
import math
from pathlib import Path
import struct
import sys


# Measured 2026-10-20: stored bbox of NW_HARBOUR_BARREL_01.MRM in Gothic II's
# Meshes.vdf, centimetres in Gothic axes. These six numbers preserve dimensions
# and the position of the origin; no original vertices or pixels are copied.
GOTHIC_MIN = (-39.9873046875, -55.88946533203125, -40.013084411621094)
GOTHIC_MAX = (39.85627746582031, 54.89350128173828, 39.83049774169922)
STAVES = 16
RING_HEIGHTS = (0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0)
TEXTURE_SIZE = 256


def make_glb(radius_scale=1.0):
    """Build glTF in metres, with right-handed CCW faces and outward normals."""
    if not math.isfinite(radius_scale) or radius_scale <= 0:
        raise ValueError("radius scale must be finite and greater than zero")
    low = (-GOTHIC_MAX[0] / 100, GOTHIC_MIN[1] / 100, GOTHIC_MIN[2] / 100)
    high = (-GOTHIC_MIN[0] / 100, GOTHIC_MAX[1] / 100, GOTHIC_MAX[2] / 100)
    cx, cz = (low[0] + high[0]) / 2, (low[2] + high[2]) / 2
    rx, rz = (high[0] - low[0]) / 2, (high[2] - low[2]) / 2
    rx, rz = rx * radius_scale, rz * radius_scale
    height = high[1] - low[1]
    positions, normals, uvs, indices = [], [], [], []

    def vertex(position, normal, uv):
        positions.append(position)
        normals.append(normal)
        uvs.append(uv)
        return len(positions) - 1

    for stave in range(STAVES):
        first = len(positions)
        # Separate UV edges for sixteen staves. The middle ring reaches the
        # measured bbox; narrower ends and analytic normals form the belly.
        for t in RING_HEIGHTS:
            radius = 0.86 + 0.14 * math.sin(math.pi * t)
            slope = 0.14 * math.pi * math.cos(math.pi * t)
            for edge in (stave, stave + 1):
                angle = math.tau * edge / STAVES
                co, si = math.cos(angle), math.sin(angle)
                normal = (co / rx, -slope / height, si / rz)
                length = math.sqrt(sum(n * n for n in normal))
                vertex((cx + rx * radius * co, low[1] + height * t,
                        cz + rz * radius * si),
                       tuple(n / length for n in normal),
                       (edge / STAVES, 0.75 * (1 - t)))
        for ring in range(len(RING_HEIGHTS) - 1):
            a = first + ring * 2
            b, c, d = a + 2, a + 3, a + 1
            indices.extend((a, b, c, a, c, d))

    for top in (False, True):
        y = high[1] if top else low[1]
        normal = (0.0, 1.0 if top else -1.0, 0.0)
        center = vertex((cx, y, cz), normal, (0.5, 0.875))
        rim = []
        for edge in range(STAVES):
            angle = math.tau * edge / STAVES
            co, si = math.cos(angle), math.sin(angle)
            rim.append(vertex((cx + rx * 0.86 * co, y, cz + rz * 0.86 * si),
                              normal, (0.5 + 0.48 * co, 0.875 + 0.115 * si)))
        for edge in range(STAVES):
            a, b = rim[edge], rim[(edge + 1) % STAVES]
            indices.extend((center, b, a) if top else (center, a, b))

    binary = bytearray()
    views, accessors = [], []

    def accessor(values, code, components, target):
        binary.extend(b"\0" * (-len(binary) % 4))
        start = len(binary)
        layout = "<" + ("f" if code == 5126 else "H") * components
        rows = values if components > 1 else [(value,) for value in values]
        stored = []
        for value in rows:
            packed = struct.pack(layout, *value)
            binary.extend(packed)
            stored.append(struct.unpack(layout, packed))
        views.append({"buffer": 0, "byteOffset": start,
                      "byteLength": len(binary) - start, "target": target})
        accessors.append({"bufferView": len(views) - 1, "componentType": code,
                          "count": len(rows),
                          "type": "SCALAR" if components == 1 else f"VEC{components}",
                          "min": [min(row[i] for row in stored) for i in range(components)],
                          "max": [max(row[i] for row in stored) for i in range(components)]})
        return len(accessors) - 1

    position = accessor(positions, 5126, 3, 34962)
    normal = accessor(normals, 5126, 3, 34962)
    uv = accessor(uvs, 5126, 2, 34962)
    index = accessor(indices, 5123, 1, 34963)
    binary.extend(b"\0" * (-len(binary) % 4))
    document = {
        "asset": {"version": "2.0", "generator": "Gothic procedural test barrel"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "OWN_TEST_BARREL"}],
        "meshes": [{"name": "OWN_TEST_BARREL", "primitives": [{
            "attributes": {"POSITION": position, "NORMAL": normal, "TEXCOORD_0": uv},
            "indices": index, "material": 0, "mode": 4}]}],
        "materials": [{"name": "OWN_TEST_BARREL_WOOD", "alphaMode": "OPAQUE",
                       "extras": {"group": "WOOD"},
                       "pbrMetallicRoughness": {"metallicFactor": 0, "roughnessFactor": 0.8}}],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": views, "accessors": accessors,
    }
    text = json.dumps(document, separators=(",", ":"), allow_nan=False).encode("utf-8")
    text += b" " * (-len(text) % 4)
    length = 12 + 8 + len(text) + 8 + len(binary)
    return (struct.pack("<4sII", b"glTF", 2, length)
            + struct.pack("<I4s", len(text), b"JSON") + text
            + struct.pack("<I4s", len(binary), b"BIN\0") + binary)


def wood_pixel(x, y):
    """Procedural staves, grain, hoops and cap planks, without source images."""
    stave_width = TEXTURE_SIZE // STAVES
    stave = x // stave_width
    grain = math.sin(x * 2.3 + math.sin(y * 0.11) * 1.8) * 7
    noise = ((x * 73856093 ^ y * 19349663) & 15) - 7
    variation = (stave * 29 % 17) - 8
    if y < 192:
        for hoop in (27, 66, 125, 165):
            delta = y - hoop
            if abs(delta) <= 4:
                edge = 12 if delta == -4 else -10 if delta == 4 else 0
                rivet = 22 if x % 32 in (14, 15, 16) and abs(delta) <= 1 else 0
                return tuple(max(0, min(255, c + edge + rivet + noise // 3))
                             for c in (57, 60, 58))
        seam = -34 if x % stave_width in (0, stave_width - 1) else 0
    else:
        seam = -32 if x % 32 in (0, 31) else 0
    shade = grain + noise + variation + seam
    return tuple(max(0, min(255, round(c + shade))) for c in (126, 87, 49))


def rgb565(rgb):
    r, g, b = rgb
    return ((r * 31 + 127) // 255 << 11
            | (g * 63 + 127) // 255 << 5 | (b * 31 + 127) // 255)


def decode565(value):
    return ((value >> 11 & 31) * 255 // 31,
            (value >> 5 & 63) * 255 // 63, (value & 31) * 255 // 31)


def make_dds():
    """Return one opaque DXT1/BC1 mip level in a legacy DDS container."""
    blocks = bytearray()
    for y in range(0, TEXTURE_SIZE, 4):
        for x in range(0, TEXTURE_SIZE, 4):
            pixels = [wood_pixel(x + dx, y + dy) for dy in range(4) for dx in range(4)]
            a = rgb565(max(pixels, key=sum))
            b = rgb565(min(pixels, key=sum))
            a, b = max(a, b), min(a, b)
            if a == b:
                a, b = (a, b - 1) if b else (1, 0)
            ca, cb = decode565(a), decode565(b)
            palette = (ca, cb, tuple((2 * ca[i] + cb[i]) // 3 for i in range(3)),
                       tuple((ca[i] + 2 * cb[i]) // 3 for i in range(3)))
            bits = 0
            for i, pixel in enumerate(pixels):
                closest = min(range(4), key=lambda j: sum(
                    (pixel[k] - palette[j][k]) ** 2 for k in range(3)))
                bits |= closest << (2 * i)
            blocks.extend(struct.pack("<HHI", a, b, bits))
    words = [124, 0x81007, TEXTURE_SIZE, TEXTURE_SIZE, len(blocks), 0, 0]
    words.extend([0] * 11)
    words.extend([32, 4, int.from_bytes(b"DXT1", "little"), 0, 0, 0, 0, 0])
    words.extend([0x1000, 0, 0, 0, 0])
    return b"DDS " + struct.pack("<31I", *words) + blocks


def make_test_barrel(directory, radius_scale=1.0):
    target = Path(directory).expanduser().resolve()
    working = (Path.home() / ".local/share/gothic-ai/content").resolve()
    in_working = target == working or working in target.parents
    if not in_working and working.exists():
        # resolve() follows symlinks but preserves case on a case-insensitive
        # filesystem; compare directory identities to catch those aliases too.
        in_working = any(candidate.exists() and candidate.samefile(working)
                         for candidate in (target, *target.parents))
    if in_working:
        raise ValueError("refusing to write into the working content directory")
    if target.exists() and (not target.is_dir() or next(target.iterdir(), None) is not None):
        raise ValueError("output directory must be new or empty")
    glb = make_glb(radius_scale)
    target.mkdir(parents=True, exist_ok=True)
    (target / "meshes/test").mkdir(parents=True)
    (target / "materials/test").mkdir(parents=True)
    (target / "meshes/test/nw_harbour_barrel_01.glb").write_bytes(glb)
    (target / "materials/test/barrel_wood.dds").write_bytes(make_dds())
    (target / "meshes/meshes.json").write_text(json.dumps({
        "NW_HARBOUR_BARREL_01": {"glb": "test/nw_harbour_barrel_01.glb"}
    }, indent=2) + "\n", encoding="utf-8")
    (target / "materials/materials.json").write_text(json.dumps({
        "OWN_TEST_BARREL_WOOD.TGA": {"albedo": "test/barrel_wood.dds"}
    }, indent=2) + "\n", encoding="utf-8")
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="new or empty test content directory")
    parser.add_argument("--radius-scale", type=float, default=1.0,
                        help="scale X/Z about the barrel axis, preserving height and origin (default: 1)")
    args = parser.parse_args()
    try:
        make_test_barrel(args.directory, radius_scale=args.radius_scale)
    except (OSError, ValueError) as error:
        print(f"make_test_barrel: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
