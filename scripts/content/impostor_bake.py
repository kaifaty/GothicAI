#!/usr/bin/env python3
"""Bake an own mesh's last triangle level and append its impostor to the set.

    uv run --no-project --with numpy --with scipy --with pillow python \
        scripts/content/impostor_bake.py SET OWN_TREE_FIR_A_4 --px 49

SET contains meshes/meshes.json and materials/materials.json. The bake reads
the mesh's UV-mapped albedo, normal and ORM DDS, with the same cutout, sidedness
and foliage inputs as the G-buffer. It renders at rest in Gothic local axes.
The frame grid and packed channels match shader/materials/impostor.*.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import importlib.util
import io
import json
import math
import re
import struct
from dataclasses import dataclass
from pathlib import Path
from tempfile import NamedTemporaryFile

import numpy as np
from PIL import Image
from scipy.ndimage import distance_transform_edt
from scipy.spatial.transform import Rotation

from world.chunk_glb import accessor, colour, read_glb


FRAMES = 12
TILE = 128
PADDING = 16
MIPS = 5
SUPERSAMPLE = 2
LAYERS = ('albedo', 'surface', 'orm')
MIRROR_X = np.array((-1., 1., 1.))


def unit(vectors: np.ndarray) -> np.ndarray:
    return vectors / np.maximum(np.linalg.norm(vectors, axis=-1, keepdims=True), 1e-12)


def oct_encode(vectors: np.ndarray) -> np.ndarray:
    vectors = vectors / np.maximum(np.abs(vectors).sum(axis=-1, keepdims=True), 1e-12)
    xz = vectors[..., (0, 2)]
    folded = (1 - np.abs(xz[..., ::-1])) * np.where(xz < 0, -1, 1)
    return np.where(vectors[..., 1:2] < 0, folded, xz) * .5 + .5


def oct_direction(x: float, z: float) -> np.ndarray:
    y = 1 - abs(x) - abs(z)
    if y < 0:
        x, z = (1 - abs(z)) * (-1 if x < 0 else 1), (1 - abs(x)) * (-1 if z < 0 else 1)
    return unit(np.array((x, y, z)))


def matrix_of(node: dict) -> np.ndarray:
    if 'matrix' in node:
        return np.array(node['matrix']).reshape(4, 4).T
    result = np.eye(4)
    result[:3, :3] = Rotation.from_quat(node.get('rotation', (0, 0, 0, 1))).as_matrix() * node.get('scale', (1, 1, 1))
    result[:3, 3] = node.get('translation', (0, 0, 0))
    return result


def mesh_primitives(path: Path):
    """The static, UV-mapped triangle subset of the default GLB scene."""
    document, binary = read_glb(path)
    nodes = document['nodes']
    pending = [(index, np.eye(4)) for index in document['scenes'][document.get('scene', 0)]['nodes']]
    visited = set()
    while pending:
        index, parent = pending.pop()
        node = nodes[index]
        if index in visited or 'skin' in node:
            raise ValueError(f'{path}: expected a static mesh in a tree of scene nodes')
        visited.add(index)
        transform = parent @ matrix_of(node)
        pending.extend((child, transform) for child in node.get('children', ()))
        if 'mesh' not in node:
            continue
        linear = transform[:3, :3]
        for primitive in document['meshes'][node['mesh']]['primitives']:
            if primitive.get('mode', 4) != 4 or primitive.get('targets'):
                raise ValueError(f'{path}: expected static triangle primitives')
            attrs = primitive['attributes']
            positions = accessor(document, binary, attrs['POSITION'])
            positions = (positions @ linear.T + transform[:3, 3]) * (100 * MIRROR_X)
            normals = accessor(document, binary, attrs['NORMAL'])
            normals = unit((normals @ np.linalg.inv(linear)) * MIRROR_X)
            uv = accessor(document, binary, attrs['TEXCOORD_0']) if 'TEXCOORD_0' in attrs else np.zeros((len(positions), 2))
            if uv.dtype.kind == 'u':
                uv = uv.astype(np.float32) / np.iinfo(uv.dtype).max
            openness = colour(document, binary, attrs['COLOR_0'])[:, 3] if 'COLOR_0' in attrs else np.ones(len(positions))
            openness = np.rint(np.clip(openness, 0, 1) * 255) / 255
            indices = accessor(document, binary, primitive['indices']) if 'indices' in primitive else np.arange(len(positions))
            indices = indices.reshape(-1, 3)
            if np.linalg.det(linear) < 0:
                indices = indices[:, (0, 2, 1)]
            material = document['materials'][primitive['material']]
            yield positions, normals, uv, openness, indices, material


def path_in(root: Path, name: str) -> Path:
    path = Path(name)
    if not name or path.is_absolute() or '..' in path.parts:
        raise ValueError(f'{name!r}: expected a path inside {root}')
    return root / path


def dds_mips(path: Path) -> list[np.ndarray]:
    """Decode the stored mips, including the cutout's per-mip coverage."""
    data = path.read_bytes()
    if data[:4] != b'DDS ' or data[84:88] not in (b'DXT1', b'DXT5'):
        raise ValueError(f'{path}: expected a DXT1 or DXT5 DDS')
    height, width = struct.unpack_from('<II', data, 12)
    levels, = struct.unpack_from('<I', data, 28)
    block = 8 if data[84:88] == b'DXT1' else 16
    offset = 128
    header = bytearray(data[:offset])
    images = []
    for _ in range(max(1, levels)):
        size = ((width + 3) // 4) * ((height + 3) // 4) * block
        struct.pack_into('<III', header, 12, height, width, size)
        struct.pack_into('<I', header, 28, 1)
        with Image.open(io.BytesIO(header + data[offset:offset + size])) as image:
            images.append(np.array(image.convert('RGBA')))
        offset += size
        width, height = max(1, width // 2), max(1, height // 2)
    return images


def sample(mips: list[np.ndarray], uv: np.ndarray, duv: np.ndarray) -> np.ndarray:
    size = np.array(mips[0].shape[:2][::-1])
    spread = np.linalg.norm(duv * size, axis=1).max()
    level = min(max(math.log2(max(spread, 1)), 0), len(mips) - 1)

    def bilinear(image: np.ndarray) -> np.ndarray:
        height, width = image.shape[:2]
        pos = uv * (width, height) - .5
        xy = np.floor(pos).astype(np.int64)
        u, v = (pos - xy).T[..., None]
        x, y = xy.T
        top = (1 - u) * image[y % height, x % width] + u * image[y % height, (x + 1) % width]
        bottom = (1 - u) * image[(y + 1) % height, x % width] + u * image[(y + 1) % height, (x + 1) % width]
        return ((1 - v) * top + v * bottom) / 255

    lower = math.floor(level)
    a = bilinear(mips[lower])
    return a if level == lower else a + (level - lower) * (bilinear(mips[lower + 1]) - a)


@dataclass
class Primitive:
    positions: np.ndarray
    normals: np.ndarray
    uv: np.ndarray
    albedo: list[np.ndarray]
    normal: list[np.ndarray] | None
    orm: list[np.ndarray] | None
    class_code: int
    transmission: float
    cutout: bool
    sides: str


def draw_primitives(path: Path, materials: Path, center: np.ndarray, radius: float) -> list[Primitive]:
    records = {key.upper(): val for key, val in json.loads((materials / 'materials.json').read_text()).items()}
    maps = {}
    result = []
    for pos, normals, uv, openness, triangles, source in mesh_primitives(path):
        key = source['name']
        key = (key if Path(key).suffix else key + '.TGA').upper()
        record = records.get(key, {})
        if record.get('antitile', 'off') != 'off' or any(record.get(field) for field in
                ('projection', 'emissive', 'window', 'hair', 'strand', 'eye', 'grass', 'fringe', 'verge', 'track')):
            raise ValueError(f'{key}: the impostor baker supports plain UV-mapped materials')
        if 'albedo' not in record or source.get('alphaMode', 'OPAQUE') not in ('OPAQUE', 'MASK'):
            raise ValueError(f'{key}: the impostor baker requires an opaque or cutout material with its own albedo DDS')
        for field in ('albedo', 'normal', 'orm'):
            name = record.get(field)
            if name is not None and name not in maps:
                maps[name] = dds_mips(path_in(materials, name))
        foliage = record.get('foliage', {})
        through = math.floor(min(1, max(0, foliage.get('translucency', .6 if foliage else 0))) * 255 + .5) / 255
        crown = foliage.get('anchor') == 'tree'
        if crown:
            normals *= .5 + .5 * openness[:, None]
        result.append(Primitive((pos[triangles] - center) / radius, normals[triangles],
                                uv[triangles] * record.get('scale', 1), maps[record['albedo']],
                                maps.get(record.get('normal')), maps.get(record.get('orm')),
                                (3 if crown else 1) + int(through > 0), through,
                                source.get('alphaMode') == 'MASK', record.get('two_sided', 'off')))
    return result


def alpha_threshold(x: np.ndarray, y: np.ndarray, uv: np.ndarray) -> np.ndarray:
    # A fixed set of subpixel tests lets near and far cards both contribute to coverage.
    cell = np.floor(uv * 256).astype(np.int32).astype(np.uint32) & 0xffff
    key = cell[:, 0] ^ (cell[:, 1] << 16) ^ (x.astype(np.uint32) * 73856093) ^ (y.astype(np.uint32) * 19349663)
    value = (key ^ (key >> 16)) * np.uint32(0x7feb352d)
    value = (value ^ (value >> 15)) * np.uint32(0x846ca68b)
    return ((value ^ (value >> 16)) >> 8) / 16777216


def render(primitives: list[Primitive], direction: np.ndarray):
    right = np.cross((0, 1, 0), direction)
    right = unit(right) if np.linalg.norm(right) > 1e-8 else np.array((1., 0., 0.))
    up = np.cross(direction, right)
    side = TILE * SUPERSAMPLE
    extent = (TILE - 2 * PADDING) * SUPERSAMPLE / 2
    zbuffer = np.full((side, side), -np.inf, np.float32)
    values = np.zeros((side, side, 11), np.float32)  # Linear colour, local normal, depth, openness, ORM.
    classes = np.zeros((side, side), np.uint8)
    for prim in primitives:
        pixels = (prim.positions @ np.stack((right, -up), axis=1)) * extent + side / 2
        depths = prim.positions @ direction
        start = np.clip(np.ceil(pixels.min(axis=1) - .5).astype(int), 0, side)
        end = np.clip(np.floor(pixels.max(axis=1) + .5).astype(int), 0, side)
        edge0, edge1 = pixels[:, 1] - pixels[:, 0], pixels[:, 2] - pixels[:, 0]
        area = edge0[:, 0] * edge1[:, 1] - edge0[:, 1] * edge1[:, 0]
        facing = np.abs(area) > 1e-9 if prim.sides in ('same', 'flip') else area > 1e-9
        candidates = np.flatnonzero(facing & (end > start).all(axis=1))
        for t in candidates:
            x0, y0 = start[t]
            x1, y1 = end[t]
            row, column = np.mgrid[y0:y1, x0:x1]
            ox, oy = pixels[t, 0]
            dx, dy = column + .5 - ox, row + .5 - oy
            i, j = edge0[t], edge1[t]
            w1 = (j[1] * dx - j[0] * dy) / area[t]
            w2 = (i[0] * dy - i[1] * dx) / area[t]
            w0 = 1 - w1 - w2
            depth = w0 * depths[t, 0] + w1 * depths[t, 1] + w2 * depths[t, 2]
            hits = (w0 >= -1e-9) & (w1 >= -1e-9) & (w2 >= -1e-9) & (depth > zbuffer[row, column])
            if not hits.any():
                continue
            x, y = column[hits], row[hits]
            weights = np.stack((w0[hits], w1[hits], w2[hits]), axis=1)
            uv = weights @ prim.uv[t]
            gradients = np.array(((i[1] - j[1], j[1], -i[1]), (j[0] - i[0], -j[0], i[0]))) / area[t]
            duv = gradients @ prim.uv[t]
            tex = sample(prim.albedo, uv, duv)
            if prim.cutout:
                covered = tex[:, 3] > alpha_threshold(x, y, uv)
                x, y, weights, uv, tex = x[covered], y[covered], weights[covered], uv[covered], tex[covered]
                if len(x) == 0:
                    continue
            zbuffer[y, x] = depth[y - y0, x - x0]
            encoded = tex[:, :3]
            values[y, x, :3] = np.where(encoded <= .04045, encoded / 12.92, ((encoded + .055) / 1.055) ** 2.4)
            normal = weights @ prim.normals[t]
            values[y, x, 7] = np.clip((np.linalg.norm(normal, axis=1) * 2 - 1) / .9, 0, 1) if prim.class_code >= 3 else 0
            normal = unit(normal)
            if prim.sides == 'flip' and area[t] < 0:
                normal = -normal
            if prim.normal is not None:
                tex = sample(prim.normal, uv, duv)
                xy = tex[:, (3, 1)] * 2 - 1
                nz = np.sqrt(np.maximum(0, 1 - (xy * xy).sum(axis=1)))
                dpdx, dpdy = gradients @ prim.positions[t]
                py, px = np.cross(dpdy, normal), np.cross(normal, dpdx)
                tangent = py * duv[0, 0] + px * duv[1, 0]
                bitangent = py * duv[0, 1] + px * duv[1, 1]
                length = np.maximum(np.linalg.norm(tangent, axis=1), np.linalg.norm(bitangent, axis=1))
                mapped = (tangent * xy[:, 0:1] + bitangent * xy[:, 1:2]) / np.maximum(length[:, None], 1e-12)
                normal = np.where(length[:, None] > 1e-12, unit(normal * nz[:, None] + mapped), normal)
            values[y, x, 3:6] = normal
            values[y, x, 6] = zbuffer[y, x] * .5 + .5
            values[y, x, 8:11] = (1, 1, 0) if prim.orm is None else sample(prim.orm, uv, duv)[:, :3]
            if prim.transmission > 0:
                values[y, x, 10] = prim.transmission
            classes[y, x] = prim.class_code
    return values, classes, np.isfinite(zbuffer).astype(np.float32)


def downsample(values: np.ndarray, classes: np.ndarray, alpha: np.ndarray):
    side = alpha.shape[0] // 2
    weights = alpha.reshape(side, 2, side, 2)
    total = weights.sum(axis=(1, 3))
    blended = (values.reshape(side, 2, side, 2, -1) * weights[..., None]).sum(axis=(1, 3))
    votes = np.stack([(weights * (classes == code).reshape(weights.shape)).sum(axis=(1, 3)) for code in range(5)], axis=-1)
    return blended / np.maximum(total[..., None], 1e-9), votes.argmax(axis=-1), total / 4


def packed(values: np.ndarray, classes: np.ndarray, alpha: np.ndarray) -> tuple[np.ndarray, ...]:
    # Only dilate attributes. Any filtered edge texel keeps its own coverage.
    mask = alpha > 0
    if mask.any() and not mask.all():
        y, x = distance_transform_edt(~mask, return_distances=False, return_indices=True)
        values, classes = values[y, x], classes[y, x]
    rgb = np.clip(values[..., :3], 0, 1)
    srgb = np.where(rgb <= .0031308, 12.92 * rgb, 1.055 * rgb ** (1 / 2.4) - .055)
    maps = (np.dstack((srgb, alpha)), np.dstack((oct_encode(values[..., 3:6]), values[..., 7], values[..., 6])),
            np.dstack((values[..., 8:11], classes * .25)))
    return tuple(np.rint(np.clip(image, 0, 1) * 255).astype(np.uint8) for image in maps)


def bake_atlases(primitives: list[Primitive]) -> tuple[bytes, ...]:
    mip_atlases = [[np.zeros((FRAMES * (TILE >> mip), FRAMES * (TILE >> mip), 4), np.uint8)
                    for mip in range(MIPS)] for _ in LAYERS]
    directions = np.linspace(-1, 1, FRAMES)
    for row, z in enumerate(directions):
        for column, x in enumerate(directions):
            frame = render(primitives, oct_direction(x, z))
            frame = downsample(*frame)
            for mip in range(MIPS):
                side = TILE >> mip
                tile = np.s_[row * side:(row + 1) * side, column * side:(column + 1) * side]
                for atlas, pixels in zip(mip_atlases, packed(*frame)):
                    atlas[mip][tile] = pixels
                if mip + 1 < MIPS:
                    frame = downsample(*frame)
    spec = importlib.util.spec_from_file_location('dds_encoder', Path(__file__).with_name('test-material.py'))
    encoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(encoder)
    size = FRAMES * TILE
    header = [124, 0xA1007, size, size, (size // 4) ** 2 * 16, 0, MIPS] + [0] * 11
    header += [32, 4, int.from_bytes(b'DXT5', 'little'), 0, 0, 0, 0, 0, 0x401008, 0, 0, 0, 0]
    prefix = b'DDS ' + struct.pack('<31I', *header)
    return tuple(prefix + b''.join(encoder.bc3(encoder.blocks(image)).tobytes() for image in atlas) for atlas in mip_atlases)


def atomic_write(path: Path, data: bytes) -> None:
    with NamedTemporaryFile(dir=path.parent, prefix=path.name + '.', delete=False) as output:
        tmp = Path(output.name)
        try:
            output.write(data)
            output.close()
            tmp.replace(path)
        finally:
            tmp.unlink(missing_ok=True)


def bake(content: Path, visual: str, px: float) -> dict:
    # Lock a stable file beside the index, whose inode changes on each atomic write.
    with (content / 'meshes' / 'meshes.json.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return _bake(content, visual, px)


def _bake(content: Path, visual: str, px: float) -> dict:
    meshes = content / 'meshes'
    index = meshes / 'meshes.json'
    records = json.loads(index.read_text())
    record = records[visual]
    levels = record['levels']
    previous = levels.pop()['impostor'] if levels and 'impostor' in levels[-1] else {}
    if not levels or not math.isfinite(px) or not 0 < px < levels[-1]['px']:
        raise ValueError('the impostor threshold must be positive and below the final triangle level')
    full = path_in(meshes, record['glb'])
    coarse = path_in(meshes, levels[-1]['glb'])
    positions = np.concatenate([part[0] for part in mesh_primitives(full)])
    low, high = positions.min(axis=0), positions.max(axis=0)
    center = (low + high) * .5
    radius = np.linalg.norm(high - low) * .5
    if not np.isfinite(radius) or radius <= 0:
        raise ValueError(f'{full}: non-finite or empty bounds')
    primitives = draw_primitives(coarse, content / 'materials', center, radius)
    atlas_bytes = bake_atlases(primitives)
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in zip(LAYERS, atlas_bytes)}
    atlases = {}
    for name, data in zip(LAYERS, atlas_bytes):
        path = full.with_name(f'{full.stem}.impostor-{name}-{hashes[name][:16]}.dds')
        if not path.exists() or path.read_bytes() != data:
            atomic_write(path, data)
        atlases[name] = path.relative_to(meshes).as_posix()
    levels.append(dict(impostor=atlases, px=px))
    atomic_write(index, (json.dumps(records, indent=2) + '\n').encode())
    if previous and full.parent.resolve().is_relative_to(meshes.resolve()):
        referenced = {Path(path) for record in records.values() if record.get('levels')
                      for path in record['levels'][-1].get('impostor', {}).values()}
        for layer in LAYERS:
            if layer not in previous:
                continue
            relative = Path(previous[layer])
            if relative.is_absolute() or relative in referenced:
                continue
            path = meshes / relative
            pattern = re.escape(f'{full.stem}.impostor-{layer}-') + r'[0-9a-f]{16}\.dds'
            if path.parent == full.parent and re.fullmatch(pattern, path.name):
                path.unlink(missing_ok=True)
    return dict(visual=visual, source=coarse.relative_to(meshes).as_posix(), px=px, atlases=atlases,
                sha256=hashes, size=FRAMES * TILE, frames=FRAMES**2, mips=MIPS,
                triangles=sum(len(prim.positions) for prim in primitives))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('set', type=Path)
    parser.add_argument('visual')
    parser.add_argument('--px', type=float, required=True)
    args = parser.parse_args()
    print(json.dumps(bake(args.set, args.visual, args.px), indent=2))


if __name__ == '__main__':
    main()
