"""The census behind the note on the cart track's own material (README.md beside this file).

Reads the base's chunks of the world and the five road textures and prints what the note quotes: how much of each
road there is, what lies beside it, how its triangles are mapped (metres per unit of u and of v, mirrored pieces,
u outside 0..1), where the T-cross and the end piece lie in the pilot, and the colours of the painted texture by
bands of u. Nothing is written.

    uv run --no-project --with numpy --with pillow python reports/engine/2026-11-15-meadow-road/road_census.py \
        ~/.cache/gothic-ai/base/<fingerprint>
"""
from __future__ import annotations

import collections
import colorsys
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

MEADOW = 'NW_NATURE_WALDBODEN_03'
ROADS = ('NW_NATURE_PATH_06', 'NW_NATURE_PATH_06_TCROSS', 'NW_NATURE_PATH_06_END', 'NW_NATURE_PATH_05',
         'NW_NATURE_PATH_05_TO_06')
PILOT = ((4, 7), (-2, 1))   # the pilot's rectangle in chunks: x, then z
LUMA = np.array([.2126, .7152, .0722])
TYPES = {5126: 'f4', 5123: 'u2', 5125: 'u4', 5121: 'u1', 5120: 'i1', 5122: 'i2'}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}
# bands of u of the painted textures, read off their column profile
BANDS = {
    'NW_NATURE_PATH_06': (('earth', .14, .86), ('middle', .47, .56), ('left rut', .28, .44), ('right rut', .59, .72),
                          ('left shoulder', .09, .22), ('right shoulder', .81, .91), ('painted grass', 0., .06)),
    'NW_NATURE_PATH_05': (('earth', .22, .78), ('painted grass', 0., .15)),
    MEADOW: (('all', 0., 1.),),
}


def in_pilot(chunk: tuple[int, int]) -> bool:
    return PILOT[0][0] <= chunk[0] <= PILOT[0][1] and PILOT[1][0] <= chunk[1] <= PILOT[1][1]


def primitives(path: Path):
    """(material's name, positions in cm, uv, triangle indices) of a chunk's GLB; x is the game's x negated."""
    data = path.read_bytes()
    length = struct.unpack('<I', data[12:16])[0]
    gltf = json.loads(data[20:20 + length])
    start = 20 + length + 8

    def accessor(index):
        a = gltf['accessors'][index]
        view = gltf['bufferViews'][a['bufferView']]
        width = WIDTH[a['type']]
        values = np.frombuffer(data, dtype='<' + TYPES[a['componentType']], count=a['count'] * width,
                               offset=start + view.get('byteOffset', 0) + a.get('byteOffset', 0))
        return values.reshape(a['count'], width) if width > 1 else values

    for p in gltf['meshes'][0]['primitives']:
        yield (gltf['materials'][p['material']]['name'], accessor(p['attributes']['POSITION']),
               accessor(p['attributes']['TEXCOORD_0']), accessor(p['indices']).reshape(-1, 3))


def quantile(values: np.ndarray, weights: np.ndarray, share: float) -> float:
    order = np.argsort(values)
    cumulative = np.cumsum(weights[order]) / weights.sum()
    return float(values[order][min(np.searchsorted(cumulative, share), len(values) - 1)])


def mapping(name: str, scope: str, pos: np.ndarray, uv: np.ndarray) -> None:
    e1, e2 = pos[:, 1] - pos[:, 0], pos[:, 2] - pos[:, 0]
    d1, d2 = uv[:, 1] - uv[:, 0], uv[:, 2] - uv[:, 0]
    det = d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]
    ok = np.abs(det) > 1e-9
    e1, e2, d1, d2, det = e1[ok], e2[ok], d1[ok], d2[ok], det[ok]
    along_u = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) / det[:, None]
    along_v = (-e1 * d2[:, 0:1] + e2 * d1[:, 0:1]) / det[:, None]
    per_u, per_v = np.linalg.norm(along_u, axis=1) / 100, np.linalg.norm(along_v, axis=1) / 100
    normal = np.cross(e1, e2)
    area = .5 * np.linalg.norm(normal, axis=1) / 1e4
    skew = np.abs((along_u * along_v).sum(1)) / (per_u * per_v * 1e4)
    slope = np.degrees(np.arccos(np.clip(np.abs(normal[:, 1]) / (2 * area * 1e4), 0, 1)))
    shares = (.05, .25, .5, .75, .95)
    print(f'{name} [{scope}]: {len(pos)} triangles, {area.sum():.0f} m2, {int((~ok).sum())} with a degenerate uv')
    print('   m per u, p5/p25/median/p75/p95:', ' '.join(f'{quantile(per_u, area, s):.2f}' for s in shares))
    print('   m per v, p5/p25/median/p75/p95:', ' '.join(f'{quantile(per_v, area, s):.2f}' for s in shares))
    print(f'   |cos(u,v)| median {quantile(skew, area, .5):.2f}, p90 {quantile(skew, area, .9):.2f};'
          f' mirrored share of the area {area[det < 0].sum() / area.sum():.2f}')
    print(f'   u {uv[..., 0].min():.2f}..{uv[..., 0].max():.2f}, v {uv[..., 1].min():.2f}..{uv[..., 1].max():.2f};'
          f' corners with u inside 0.02..0.98: {int(((uv[..., 0] > .02) & (uv[..., 0] < .98)).sum())} of {uv[..., 0].size}')
    print(f'   slope, degrees: median {quantile(slope, area, .5):.1f}, p90 {quantile(slope, area, .9):.1f},'
          f' max {slope.max():.1f}')


def colours(base: Path) -> None:
    def linear(value):
        value = value / 255.
        return np.where(value <= .04045, value / 12.92, ((value + .055) / 1.055) ** 2.4)

    for name, bands in BANDS.items():
        image = linear(np.asarray(Image.open(base / 'textures' / f'{name}.dds').convert('RGB'), dtype=np.float64))
        width = image.shape[1]
        print(f'{name}: {image.shape[1]}x{image.shape[0]}, mean luminance {float((image @ LUMA).mean()):.4f}')
        for band, left, right in bands:
            mean = image[:, int(left * width):int(right * width)].reshape(-1, 3).mean(0)
            hue, saturation, _ = colorsys.rgb_to_hsv(*mean)
            print(f'   {band} (u {left:.2f}-{right:.2f}): rgb {mean[0]:.4f} {mean[1]:.4f} {mean[2]:.4f},'
                  f' luminance {float(mean @ LUMA):.4f}, hue {hue * 360:.1f}, saturation {saturation:.2f}')


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    base = Path(sys.argv[1]).expanduser()
    chunks = sorted((base / 'worlds/NEWWORLD/chunks').glob('*.glb'))
    if not chunks:
        print(f'no chunks under {base}')
        return 1
    edges: dict = {}
    triangles = collections.defaultdict(list)
    for path in chunks:
        chunk = tuple(int(part) for part in path.stem.split('_'))
        for name, pos, uv, index in primitives(path):
            corners = pos[index]
            whole = np.round(corners).astype(np.int64)
            for t in range(len(index)):
                for e in range(3):
                    a, b = tuple(whole[t, e]), tuple(whole[t, (e + 1) % 3])
                    edges.setdefault((a, b) if a < b else (b, a), []).append((name, chunk))
            if name in ROADS:
                triangles[name].append((chunk, corners.astype(np.float64), uv[index].astype(np.float64)))

    print('== how much of each road, and how it is mapped')
    for name in ROADS:
        for scope in ('pilot', 'island'):
            kept = [(c, p, u) for c, p, u in triangles[name] if scope == 'island' or in_pilot(c)]
            if kept:
                mapping(name, scope, np.concatenate([p for _, p, _ in kept]), np.concatenate([u for _, _, u in kept]))
        print(f'   chunks: {len(triangles[name])}, in the pilot {sum(in_pilot(c) for c, _, _ in triangles[name])}')

    print('== what lies beside, by the length of the shared edges, m')
    for scope in ('pilot', 'island'):
        beside = collections.defaultdict(lambda: collections.defaultdict(float))
        for (a, b), owners in edges.items():
            names = {n for n, c in owners if scope == 'island' or in_pilot(c)}
            if len(names) < 2:
                continue
            length = float(np.linalg.norm(np.array(a) - np.array(b))) / 100
            for road in names.intersection(ROADS):
                for other in names - {road}:
                    beside[road][other] += length
        for road in ROADS:
            total = sum(beside[road].values())
            print(f'{road} [{scope}] {total:.0f}: ' + ', '.join(
                f'{other.replace("NW_NATURE_", "")} {length:.0f}'
                for other, length in sorted(beside[road].items(), key=lambda item: -item[1])[:12]))

    print("== the pilot's pieces, the game's coordinates in cm (x of the GLB negated)")
    for name in ROADS[1:]:
        for chunk, pos, _ in triangles[name]:
            if in_pilot(chunk):
                centre = pos.reshape(-1, 3).mean(0)
                print(f'{name} chunk {chunk[0]}_{chunk[1]}: {len(pos)} triangles around'
                      f' {-centre[0]:.0f} {centre[1]:.0f} {centre[2]:.0f}')

    print('== the painted textures by bands of u, linear')
    colours(base)
    return 0


if __name__ == '__main__':
    sys.exit(main())
