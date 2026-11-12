"""Census of a region of a converted world: triangles, area, material classes, slopes, surface layers and objects.

    python3 scripts/content/world/world_census.py NEWWORLD [--base DIR] [--content DIR]
        [--chunks 8_-4,8_-3 | --rect X0,Z0,X1,Z1] [--cell METRES] [--out DIR]

Prints a Markdown report; with --out writes <WORLD>_census.md and <WORLD>_census.json there. The base and the
content folder are only read. Without --chunks/--rect the region is the whole world.

Definitions are those of the research census (reports/engine/2026-11-12-world-detail/research/world_census.md,
"How to read the numbers"); the class rules are in world_classes.py. In short:
- positions are the chunk's glTF metres (glTF x = -Gothic x); the report's bounding box is in Gothic axes;
- area is the 3D surface of the triangles; plan area is sum(area x normal y) over up-facing triangles;
- a triangle is below sea when its centroid is lower than the sea level: the median centroid height of the world's
  largest water material, taken over the whole world whatever the region;
- slope is the angle of the face normal from straight up, area-weighted; a face exactly on a band edge belongs to
  the steeper band (angles are rounded to 1e-6 degrees); "leaning out" is normal y in [-0.5, 0), "ceiling" < -0.5;
- layers: every up-facing triangle (normal y > 0.1) of a set gives a height at each grid cell whose centre it
  covers; a cell's heights sorted and chained, a gap of more than 2 m starts a new layer;
- objects: vobs of vobs.json with all children, by the class of their visual name; with a region, a vob belongs to
  the chunk of its position.

With --content DIR, a chunk <DIR>/worlds/<WORLD>/chunks/<x>_<z>.glb replaces the base chunk of the same name (as
OpenGothic/common/worldchunksruntime.cpp does), <DIR>/worlds/<WORLD>/vobs.json replaces the base vobs.json, the
vob layers <DIR>/worlds/<WORLD>/vobs.d/*.json are applied in name order (docs/asset-migration.md, «Данные мира»)
and the meshes of <DIR>/meshes/meshes.json replace base meshes of the same name for the triangle counts.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import chunk_glb as cg  # noqa: E402
import world_classes as wc  # noqa: E402

CHUNK_M = 64.0
LAYER_GAP_M = 2.0
UP_NORMAL_Y = 0.1
SLOPE_BANDS = ((0, 15), (15, 30), (30, 45), (45, 60), (60, 90))
RASTER_BATCH = 3_000_000      # candidate (triangle, cell) pairs per batch of the raster


# --- where the data is

def resolve_base(given: str | None) -> Path:
    """--base, else OPENGOTHIC_BASE, else the newest directory with base.json under ~/.cache/gothic-ai/base."""
    if given:
        return Path(given).expanduser()
    if os.environ.get('OPENGOTHIC_BASE'):
        return Path(os.environ['OPENGOTHIC_BASE']).expanduser()
    cache = Path.home() / '.cache' / 'gothic-ai' / 'base'
    found = [p for p in cache.iterdir() if (p / 'base.json').is_file()] if cache.is_dir() else []
    if not found:
        raise SystemExit('no base: pass --base or set OPENGOTHIC_BASE')
    return max(found, key=lambda p: (p / 'base.json').stat().st_mtime)


@dataclass
class ChunkFile:
    name: str            # '8_-4'
    path: Path
    from_content: bool

    @property
    def coords(self) -> tuple[int, int]:
        return cg.chunk_coords(self.name)


def chunk_files(base: Path, world: str, content: Path | None = None) -> list[ChunkFile]:
    """The chunks of the world's manifest, each from the content folder when it has a file of the same name."""
    wdir = base / 'worlds' / world
    manifest = json.loads((wdir / 'world.json').read_text())
    out = []
    for rel in manifest['chunks']:
        over = content / 'worlds' / world / rel if content else None
        if over is not None and over.is_file():
            out.append(ChunkFile(Path(rel).stem, over, True))
        else:
            out.append(ChunkFile(Path(rel).stem, wdir / rel, False))
    return out


def parse_chunks(text: str) -> set[tuple[int, int]]:
    return {cg.chunk_coords(s.strip()) for s in text.split(',') if s.strip()}


def parse_rect(text: str) -> set[tuple[int, int]]:
    x0, z0, x1, z1 = (int(v) for v in text.split(','))
    return {(x, z) for x in range(min(x0, x1), max(x0, x1) + 1) for z in range(min(z0, z1), max(z0, z1) + 1)}


# --- the world mesh

@dataclass
class WorldMesh:
    """Triangles of a set of chunks. positions: (t, 3, 3) float64 metres in the chunk's glTF axes."""
    positions: np.ndarray
    uvs: np.ndarray                 # (t, 3, 2) float32
    material: np.ndarray            # (t,) index into materials
    chunk: np.ndarray               # (t, 2) chunk x, z of the file the triangle is in
    materials: list[dict]           # world.json records, then new names of content chunks
    files: list[ChunkFile] = field(default_factory=list)

    def __len__(self):
        return len(self.positions)


def read_triangles(path: Path, keep=None) -> list[tuple[dict, np.ndarray, np.ndarray]]:
    """Per primitive of a chunk: material record, positions (t, 3, 3) float32 cm and uvs (t, 3, 2). keep(record)
    picks the primitives to read; the others are skipped unread."""
    doc, binary = cg.read_glb(path)
    out = []
    for mesh in doc.get('meshes', []):
        for prim in mesh['primitives']:
            if prim.get('mode', 4) != 4:
                raise ValueError(f'{path}: only triangle lists are supported')
            record = cg.material_record(doc, prim.get('material'))
            if keep is not None and not keep(record):
                continue
            attrs = prim['attributes']
            pos = cg.accessor(doc, binary, attrs['POSITION']).astype(np.float32)
            uv = cg.accessor(doc, binary, attrs['TEXCOORD_0']).astype(np.float32) if 'TEXCOORD_0' in attrs \
                else np.zeros((len(pos), 2), np.float32)
            idx = cg.accessor(doc, binary, prim['indices']).astype(np.int64) if 'indices' in prim \
                else np.arange(len(pos))
            tri = idx.reshape(-1, 3)
            out.append((record, pos[tri], uv[tri]))
    return out


def world_materials(base: Path, world: str) -> list[dict]:
    return json.loads((base / 'worlds' / world / 'world.json').read_text())['materials']


def load_world(base: Path, world: str, content: Path | None = None,
               region: set[tuple[int, int]] | None = None) -> WorldMesh:
    """The triangles of the region's chunks (all chunks without a region), content chunks replacing base ones."""
    materials = [dict(m) for m in world_materials(base, world)]
    by_name = {m['name']: i for i, m in enumerate(materials)}
    files = [f for f in chunk_files(base, world, content) if region is None or f.coords in region]
    P, UV, M, C = [], [], [], []
    for f in files:
        x, z = f.coords
        for record, pos, uv in read_triangles(f.path):
            name = record.get('name', '')
            if name not in by_name:
                by_name[name] = len(materials)
                materials.append({'name': name, 'group': record.get('group'), 'texture': record.get('texture', '')})
            P.append(pos)
            UV.append(uv)
            M.append(np.full(len(pos), by_name[name], np.int32))
            C.append(np.full((len(pos), 2), (x, z), np.int32))
    if not P:
        return WorldMesh(np.zeros((0, 3, 3)), np.zeros((0, 3, 2), np.float32), np.zeros(0, np.int32),
                         np.zeros((0, 2), np.int32), materials, files)
    return WorldMesh(np.concatenate(P).astype(np.float64) / 100.0, np.concatenate(UV), np.concatenate(M),
                     np.concatenate(C), materials, files)


def material_classes(materials: list[dict]) -> np.ndarray:
    return np.array([wc.material_class(m['name'], m.get('group')) for m in materials] or ['other'])


def sea_level(base: Path, world: str, content: Path | None = None, mesh: WorldMesh | None = None) -> float | None:
    """Median centroid height (m) of the largest water material of the whole world; None without water. A mesh
    of the whole world is used as it is; otherwise the water primitives of every chunk are read."""
    if mesh is not None and len(mesh.files) == len(chunk_files(base, world, content)):
        cls = material_classes(mesh.materials)[mesh.material]
        sel = cls == 'water'
        names = np.array([mesh.materials[i]['name'] for i in mesh.material[sel]]) if sel.any() else np.zeros(0)
        P = mesh.positions[sel]
    else:
        names, parts = [], []

        def is_water(record):
            return wc.material_class(record.get('name', ''), record.get('group')) == 'water'

        for f in chunk_files(base, world, content):
            for record, pos, _ in read_triangles(f.path, is_water):
                names += [record.get('name', '')] * len(pos)
                parts.append(pos)
        names = np.array(names)
        P = np.concatenate(parts).astype(np.float64) / 100.0 if parts else np.zeros((0, 3, 3))
    if not len(P):
        return None
    area = triangle_areas(P)[0]
    _, inv = np.unique(names, return_inverse=True)
    largest = np.bincount(inv, area).argmax()
    return float(np.median(P[inv == largest].mean(1)[:, 1]))


def triangle_areas(P: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Area (m²) and normal y of each triangle (0 for a zero-area one)."""
    n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
    nl = np.linalg.norm(n, axis=1)
    return nl / 2, np.divide(n[:, 1], nl, out=np.zeros(len(P)), where=nl > 0)


# --- height raster

def rasterize(P: np.ndarray, cell: float = 1.0) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Every (cell, height) where a triangle covers the centre of an XZ grid cell (exact barycentric test).
    P: (t, 3, 3) metres; cell i spans [i cell, (i + 1) cell) on x and z of P's axes. Returns cell indices (n, 2),
    heights (n,) and the triangle (index into P) of each hit."""
    x, z, y = P[:, :, 0] / cell, P[:, :, 2] / cell, P[:, :, 1]
    i0 = np.ceil(x.min(1) - 0.5).astype(np.int64)
    i1 = np.floor(x.max(1) - 0.5).astype(np.int64)
    j0 = np.ceil(z.min(1) - 0.5).astype(np.int64)
    j1 = np.floor(z.max(1) - 0.5).astype(np.int64)
    nx, nz = i1 - i0 + 1, j1 - j0 + 1
    ok = (nx > 0) & (nz > 0)
    idx = np.nonzero(ok)[0]
    x, z, y, i0, j0, nx, nz = (a[ok] for a in (x, z, y, i0, j0, nx, nz))
    cells, heights, tris = [np.zeros((0, 2), np.int64)], [np.zeros(0)], [np.zeros(0, np.int64)]

    def batch(sl):
        mx, mz = int(nx[sl].max()), int(nz[sl].max())
        oi, oj = np.meshgrid(np.arange(mx), np.arange(mz), indexing='ij')
        oi, oj = oi.ravel(), oj.ravel()
        ci = i0[sl][:, None] + oi[None, :]
        cj = j0[sl][:, None] + oj[None, :]
        inside = (oi[None, :] < nx[sl][:, None]) & (oj[None, :] < nz[sl][:, None])
        px, pz = ci + 0.5, cj + 0.5
        xx, zz, yy = x[sl], z[sl], y[sl]
        x0, x1, x2 = xx[:, 0:1], xx[:, 1:2], xx[:, 2:3]
        z0, z1, z2 = zz[:, 0:1], zz[:, 1:2], zz[:, 2:3]
        den = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2)
        den = np.where(den == 0, np.nan, den)
        l0 = ((z1 - z2) * (px - x2) + (x2 - x1) * (pz - z2)) / den
        l1 = ((z2 - z0) * (px - x2) + (x0 - x2) * (pz - z2)) / den
        l2 = 1 - l0 - l1
        e = -1e-9
        m = inside & (l0 >= e) & (l1 >= e) & (l2 >= e)
        h = l0 * yy[:, 0:1] + l1 * yy[:, 1:2] + l2 * yy[:, 2:3]
        r, c = np.nonzero(m)
        cells.append(np.stack([ci[r, c], cj[r, c]], 1))
        heights.append(h[r, c])
        tris.append(idx[sl][r])

    order = np.argsort(nx * nz, kind='stable')
    pos, count = 0, len(order)
    while pos < count:
        end, step = pos + 1, 256
        while end < count:
            nxt = min(count, end + step)
            sl = order[pos:nxt]
            if (nxt - pos) * int(nx[sl].max()) * int(nz[sl].max()) > RASTER_BATCH:
                if step == 1:
                    break
                step //= 2
                continue
            end = nxt
        batch(order[pos:end])
        pos = end
    return np.concatenate(cells), np.concatenate(heights), np.concatenate(tris)


@dataclass
class Layers:
    """Surface layers on the XZ grid of the positions given (for a WorldMesh: glTF axes, so cell i on x spans
    Gothic x from -(i + 1) cell to -i cell). Hits are sorted by cell, then height; the hits of cell k are
    heights[start[k]:start[k + 1]], with their triangles (indices into the positions given) in tris."""
    cell_size: float
    cells: np.ndarray       # (n, 2) cell indices
    count: np.ndarray       # (n,) layers per cell
    start: np.ndarray       # (n + 1,) offsets into heights
    heights: np.ndarray
    tris: np.ndarray

    def lowest(self) -> np.ndarray:
        return self.heights[self.start[:-1]]

    def highest(self) -> np.ndarray:
        return self.heights[self.start[1:] - 1]


def surface_layers(P: np.ndarray, cell: float = 1.0, gap: float = LAYER_GAP_M) -> Layers:
    """Layers of the triangles P per cell: heights sorted and chained, a gap over `gap` m starts a new layer."""
    cl, h, tid = rasterize(P, cell)
    if not len(cl):
        z = np.zeros(0, np.int64)
        return Layers(cell, np.zeros((0, 2), np.int64), z, np.zeros(1, np.int64), np.zeros(0), z)
    gx0, gz0 = int(cl[:, 0].min()), int(cl[:, 1].min())
    width = int(cl[:, 1].max()) - gz0 + 1
    key = (cl[:, 0] - gx0) * width + (cl[:, 1] - gz0)
    o = np.lexsort((h, key))
    key, h, tid = key[o], h[o], tid[o]
    newcell = np.r_[True, key[1:] != key[:-1]]
    newlayer = newcell | np.r_[False, (h[1:] - h[:-1]) > gap]
    cellid = np.cumsum(newcell) - 1
    first = np.nonzero(newcell)[0]
    ck = key[first]
    cells = np.stack([ck // width + gx0, ck % width + gz0], 1)
    return Layers(cell, cells, np.bincount(cellid, newlayer.astype(np.int64)), np.r_[first, len(key)], h, tid)


# --- objects

def mesh_index(base: Path, content: Path | None) -> dict[str, Path]:
    """Mesh files of the content folder by upper-case name (meshes/meshes.json)."""
    if content is None:
        return {}
    index = content / 'meshes' / 'meshes.json'
    if not index.is_file():
        return {}
    return {k.upper(): content / 'meshes' / v['glb'] for k, v in json.loads(index.read_text()).items()
            if isinstance(v, dict) and v.get('glb')}


def mesh_triangles(path: Path) -> int | None:
    try:
        doc, _ = cg.read_glb(path)
    except (OSError, ValueError):
        return None
    t = 0
    for m in doc.get('meshes', []):
        for p in m['primitives']:
            key = p['indices'] if 'indices' in p else p['attributes']['POSITION']
            t += doc['accessors'][key]['count'] // 3
    return t


@dataclass
class Vob:
    id: int | None          # None for a layer addition
    visual: str
    position: tuple         # Gothic cm
    source: str = 'base'    # 'base', 'content vobs.json' or the layer file name
    kind: str = ''          # vobs.json class (zCVob, oCMobFire, zCVobSpot, ...) or the layer addition's kind
    key: str | None = None  # the layer addition's key


def walk_vobs(vobs: list, out: list[Vob], source: str) -> None:
    for o in vobs:
        pos = o.get('position')
        out.append(Vob(o.get('id'), (o.get('visual_name') or '').upper(),
                       tuple(pos) if isinstance(pos, list) else (0, 0, 0), source, o.get('class') or ''))
        walk_vobs(o.get('children') or [], out, source)


def load_vobs(base: Path, world: str, content: Path | None = None) -> tuple[list[Vob], list[dict]]:
    """All vobs of the world with their children, after the content folder's vobs.json and vob layers. Returns the
    vobs and one summary per layer file (name, added meshes, other additions, removed, moved; a file with an
    unknown, removed or twice-used id is rejected whole, as the engine does, and lists why)."""
    path = base / 'worlds' / world / 'vobs.json'
    source = 'base'
    if content is not None and (content / 'worlds' / world / 'vobs.json').is_file():
        path, source = content / 'worlds' / world / 'vobs.json', 'content vobs.json'
    vobs: list[Vob] = []
    walk_vobs(json.loads(path.read_text())['vobs'], vobs, source)
    layers = []
    ldir = content / 'worlds' / world / 'vobs.d' if content is not None else None
    if ldir is None or not ldir.is_dir():
        return vobs, layers
    by_id = {v.id: v for v in vobs if v.id is not None}
    removed: set[int] = set()
    for f in sorted(ldir.glob('*.json')):
        doc = json.loads(f.read_text())
        row = {'file': f.name, 'meshes': 0, 'other_added': 0, 'removed': 0, 'moved': 0, 'rejected': []}
        remove = list(doc.get('remove', []))
        move = [m.get('id') for m in doc.get('move', [])]
        # The engine applies a file whole or not at all (docs/asset-migration.md): only the ids are checked here.
        for i in remove + move:
            if i not in by_id or i in removed:
                row['rejected'].append(f'unknown or removed id {i}')
        row['rejected'] += [f'id {i} removed twice' for i in sorted({i for i in remove if remove.count(i) > 1})]
        row['rejected'] += [f'id {i} removed and moved' for i in sorted(set(remove) & set(move))]
        if row['rejected']:
            layers.append(row)
            continue
        for a in doc.get('add', []):
            if a.get('kind') == 'mesh':
                vobs.append(Vob(None, str(a.get('visual', '')).upper(), tuple(a.get('position', (0, 0, 0))), f.name,
                                'mesh', a.get('key')))
                row['meshes'] += 1
            else:
                row['other_added'] += 1
        removed.update(remove)
        row['removed'] = len(remove)
        for m in doc.get('move', []):
            by_id[m['id']].position = tuple(m.get('position', by_id[m['id']].position))
        row['moved'] = len(move)
        layers.append(row)
    return [v for v in vobs if v.id is None or v.id not in removed], layers


def vob_chunk(v: Vob) -> tuple[int, int]:
    return int(np.floor(v.position[0] / (CHUNK_M * 100))), int(np.floor(v.position[2] / (CHUNK_M * 100)))


def object_census(base: Path, world: str, content: Path | None = None,
                  region: set[tuple[int, int]] | None = None) -> dict:
    vobs, layers = load_vobs(base, world, content)
    if region is not None:
        vobs = [v for v in vobs if vob_chunk(v) in region]
    own = mesh_index(base, content)
    tris_cache: dict[str, int | None] = {}
    from_content: set[str] = set()

    def tris(name: str) -> int | None:
        if name not in tris_cache:
            plain = name[:-4] if name.endswith(('.3DS', '.MDL', '.ASC', '.MRM')) else name
            stem = wc.MESH_EXTENSION.sub('', plain)
            if stem in own:
                tris_cache[name] = mesh_triangles(own[stem])
                from_content.add(stem)
            else:
                f = base / 'meshes' / f'{plain}.glb'
                tris_cache[name] = mesh_triangles(f) if f.is_file() else None
        return tris_cache[name]

    by: dict[str, Counter] = {c: Counter() for c in wc.OBJECT_CLASSES}
    for v in vobs:
        c = wc.object_class(v.visual)
        if c:
            by[c][v.visual] += 1
    rows = {}
    for c in wc.OBJECT_CLASSES:
        top = [(n, k, tris(n)) for n, k in by[c].most_common()]
        rows[c] = {'instances': sum(by[c].values()), 'distinct_meshes': len(by[c]),
                   'instances_with_mesh': sum(k for _, k, t in top if t is not None),
                   'mesh_triangles': sum(k * t for _, k, t in top if t is not None),
                   'top': [[n, k, t] for n, k, t in top[:12]]}
    return {'vobs': len(vobs), 'with_mesh_visual': sum(r['instances'] for r in rows.values()), 'classes': rows,
            'layers': layers, 'meshes_from_content': sorted(from_content)}


# --- the census

def pct(x, q=(10, 50, 90)) -> list[float]:
    return [round(float(v), 2) for v in np.percentile(x, q)] if len(x) else [None] * len(q)


def dds_size(base: Path, texture: str) -> tuple[int, int] | None:
    f = base / 'textures' / (Path(texture).stem + '.dds')
    if not texture or not f.is_file():
        return None
    h, w = struct.unpack_from('<II', f.open('rb').read(32), 12)
    return w, h


def census(base: Path, world: str, content: Path | None = None, region: set[tuple[int, int]] | None = None,
           cell: float = 1.0) -> dict:
    t0 = time.time()
    mesh = load_world(base, world, content, region)
    P, M, C, mats = mesh.positions, mesh.material, mesh.chunk, mesh.materials
    nt = len(P)
    cls_of = material_classes(mats)
    CL = cls_of[M] if nt else np.zeros(0, str)
    A, ny = triangle_areas(P)
    sea = sea_level(base, world, content, mesh if region is None else None)
    under = P.mean(1)[:, 1] < sea if sea is not None else np.zeros(nt, bool)
    land = ~under
    up = ny > 0
    plan = A * ny
    R: dict = {'world': world, 'base': str(base), 'content': str(content) if content else None,
               'region': sorted(f'{x}_{z}' for x, z in region) if region is not None else None, 'cell_m': cell,
               'sea_level_m': sea}
    all_names = {f.name for f in chunk_files(base, world, content)}
    R['missing_chunks'] = sorted(f'{x}_{z}' for x, z in region if f'{x}_{z}' not in all_names) if region else []
    R['content_chunks'] = sorted(f.name for f in mesh.files if f.from_content)

    G = P.reshape(-1, 3) * np.array([-1, 1, 1])
    R['totals'] = {'triangles': nt, 'chunk_files': len(mesh.files),
                   'bbox_min_m': G.min(0).round(1).tolist() if nt else None,
                   'bbox_max_m': G.max(0).round(1).tolist() if nt else None,
                   'surface_m2': float(A.sum()), 'plan_up_m2': float(plan[up].sum()),
                   'zero_area_triangles': int((A == 0).sum()),
                   'other_share': float((CL == 'other').sum() / nt) if nt else 0.0}

    def stats(sel):
        n = int(sel.sum())
        a = float(A[sel].sum())
        return {'triangles': n, 'area_m2': a, 'plan_m2': float(plan[sel & up].sum()),
                'tris_per_m2': n / a if a > 0 else None}

    R['classes'] = {c: {'all': stats(CL == c), 'land': stats((CL == c) & land), 'below_sea': stats((CL == c) & under)}
                    for c in wc.MATERIAL_CLASSES}

    E = np.linalg.norm(P[:, [1, 2, 0]] - P, axis=2)
    R['edges'] = {c: {part: {'triangles': int(s.sum()), 'p10_p50_p90_m': pct(E[s].ravel()),
                             'median_area_m2': round(float(np.median(A[s])), 3) if s.any() else None}
                      for part, s in (('all', CL == c), ('land', (CL == c) & land))}
                  for c in wc.MATERIAL_CLASSES if (CL == c).any()}

    cid = C[:, 0].astype(np.int64) * 100000 + C[:, 1]
    u, inv = np.unique(cid, return_inverse=True)
    ct, ca = np.bincount(inv), np.bincount(inv, A)
    R['chunks'] = {'with_triangles': len(u)}
    if len(u):
        R['chunks'].update(min=int(ct.min()), median=float(np.median(ct)), p90=float(np.percentile(ct, 90)),
                           max=int(ct.max()))
    dense = []
    for k in np.argsort(-ct, kind='stable')[:12]:
        s = inv == k
        x, z = C[s][0]
        shares = {c: float(A[s & (CL == c)].sum() / ca[k]) for c in wc.MATERIAL_CLASSES if ca[k] > 0}
        mm = np.bincount(M[s], A[s], minlength=len(mats))
        dense.append({'chunk': f'{x}_{z}', 'triangles': int(ct[k]), 'area_m2': float(ca[k]),
                      'shares': {c: v for c, v in shares.items() if v > 0.1},
                      'top_materials': [mats[j]['name'] for j in np.argsort(-mm, kind='stable')[:2]]})
    R['densest_chunks'] = dense

    ang = np.round(np.degrees(np.arccos(np.clip(ny, -1, 1))), 6)

    def slope(sel):
        tot = A[sel].sum()
        if tot <= 0:
            return None
        row = [float(A[sel & (ny >= 0) & (ang >= lo) & ((ang < hi) if hi < 90 else (ang <= 90))].sum() / tot)
               for lo, hi in SLOPE_BANDS]
        row.append(float(A[sel & (ny < 0) & (ny >= -0.5)].sum() / tot))
        row.append(float(A[sel & (ny < -0.5)].sum() / tot))
        return row

    R['slope'] = {c: {'all': slope(CL == c), 'land': slope((CL == c) & land)}
                  for c in wc.MATERIAL_CLASSES if (CL == c).any()}

    ar = np.bincount(M, A, minlength=len(mats))
    nm = np.bincount(M, minlength=len(mats))
    ground = [i for i in np.argsort(-ar, kind='stable') if cls_of[i] == 'ground' and ar[i] > 0][:10]
    texel = []
    for i in ground:
        s = (M == i) & (A > 0.05)
        uv = mesh.uvs[s].astype(np.float64)
        uva = np.abs((uv[:, 1, 0] - uv[:, 0, 0]) * (uv[:, 2, 1] - uv[:, 0, 1])
                     - (uv[:, 2, 0] - uv[:, 0, 0]) * (uv[:, 1, 1] - uv[:, 0, 1])) / 2
        ok = uva > 0
        size = dds_size(base, mats[i].get('texture', ''))
        row = {'material': mats[i]['name'], 'dds': f'{size[0]}x{size[1]}' if size else None, 'area_m2': float(ar[i])}
        if size and ok.any():
            td = np.sqrt(uva[ok] * size[0] * size[1] / A[s][ok])
            row.update(median=round(float(np.median(td)), 1), p10=round(float(np.percentile(td, 10)), 1),
                       p90=round(float(np.percentile(td, 90)), 1))
        texel.append(row)
    R['texel_density'] = texel

    R['materials'] = [{'material': mats[i]['name'], 'group': mats[i].get('group'), 'class': str(cls_of[i]),
                       'triangles': int(nm[i]), 'area_m2': float(ar[i]),
                       'median_edge_m': round(float(np.median(E[M == i])), 2)}
                      for i in np.argsort(-ar, kind='stable')[:40] if nm[i]]

    natural = np.isin(CL, wc.NATURAL) & land & (ny > UP_NORMAL_Y)
    solid = np.isin(CL, wc.NATURAL + ('man-made',)) & land & (ny > UP_NORMAL_Y)
    R['layers'] = {}
    for label, sel in (('natural', natural), ('natural + man-made', solid)):
        L = surface_layers(P[sel], cell)
        n = L.count
        gapm = L.highest() - L.lowest()
        multi = n >= 2
        R['layers'][label] = {'cells': len(n), 'one': int((n == 1).sum()), 'two_or_more': int(multi.sum()),
                              'three_or_more': int((n >= 3).sum()),
                              'multi_gap_m': {'<=5': int((gapm[multi] <= 5).sum()),
                                              '5-15': int(((gapm[multi] > 5) & (gapm[multi] <= 15)).sum()),
                                              '>15': int((gapm[multi] > 15).sum())}}
    R['objects'] = object_census(base, world, content, region)
    R['seconds'] = round(time.time() - t0, 1)
    return R


# --- report

def table(head: list[str], rows: list[list]) -> str:
    s = '| ' + ' | '.join(head) + ' |\n|' + '|'.join(['---'] * len(head)) + '|\n'
    return s + ''.join('| ' + ' | '.join('' if x is None else str(x) for x in r) + ' |\n' for r in rows) + '\n'


def num(v, digits=0) -> str:
    return '' if v is None else f'{v:,.{digits}f}'


def percent(v) -> str:
    return '' if v is None else f'{v * 100:.1f}'


def markdown(R: dict) -> str:
    w = R['world']
    t = R['totals']
    out = [f'# Census of {w}\n']
    region = 'whole world' if R['region'] is None else f'{len(R["region"])} chunks: {", ".join(R["region"])}'
    out.append(f'Base `{R["base"]}`; region {region}; grid cell {R["cell_m"]} m.')
    if R['missing_chunks']:
        out.append(f'Not in the world (skipped): {", ".join(R["missing_chunks"])}.')
    if R['content']:
        layers = R['objects']['layers']
        out.append(f'Content `{R["content"]}`: chunks from content {", ".join(R["content_chunks"]) or "none"}; '
                   f'vob layers {", ".join(L["file"] for L in layers) or "none"}; '
                   f'meshes from content {len(R["objects"]["meshes_from_content"])}.')
    sea = R['sea_level_m']
    out.append('Sea level: ' + (f'y = {sea:.1f} m (median centroid height of the world\'s largest water material); '
                                'a triangle with its centroid below it is "below sea".' if sea is not None
                                else 'no water in the world, every triangle counts as land.') + '\n')

    out.append('## Totals\n')
    out.append(table(['quantity', 'value'], [
        ['triangles', f'{t["triangles"]:,}'], ['chunk files', t['chunk_files']],
        ['bbox min x,y,z (m, Gothic axes)', t['bbox_min_m']], ['bbox max x,y,z (m, Gothic axes)', t['bbox_max_m']],
        ['surface area (m²)', num(t['surface_m2'])], ['plan area of up-facing triangles, sum(A·ny) (m²)', num(t['plan_up_m2'])],
        ['zero-area triangles', t['zero_area_triangles']],
        ['share of triangles in class `other`', f'{t["other_share"] * 100:.2f} %']]))

    out.append('## By material class\n')
    rows = []
    for c, parts in R['classes'].items():
        for part in ('all', 'land', 'below_sea') if c in wc.NATURAL else ('all',):
            s = parts[part]
            rows.append([c, part.replace('_', ' '), f'{s["triangles"]:,}', num(s['area_m2']), num(s['plan_m2']),
                         num(s['tris_per_m2'], 3)])
    out.append(table(['class', 'part', 'triangles', 'area m²', 'plan (up-facing) m²', 'tris per m²'], rows))

    out.append('## Edge length per class (m, all three edges of each triangle)\n')
    rows = [[c, part, f'{s["triangles"]:,}', *s['p10_p50_p90_m'], s['median_area_m2']]
            for c, parts in R['edges'].items() for part, s in parts.items()]
    out.append(table(['class', 'part', 'triangles', 'p10', 'p50', 'p90', 'median triangle area m²'], rows))

    ch = R['chunks']
    out.append('## Triangles per chunk\n')
    out.append(table(['chunks with triangles', 'min', 'median', 'p90', 'max'],
                     [[ch['with_triangles'], ch.get('min'), num(ch.get('median')), num(ch.get('p90')), ch.get('max')]]))
    out.append('Densest chunks:\n')
    out.append(table(['chunk', 'triangles', 'area m²', 'tris per m²', 'classes over 10 % of area', 'top materials'],
                     [[d['chunk'], f'{d["triangles"]:,}', num(d['area_m2']),
                       f'{d["triangles"] / d["area_m2"]:.2f}' if d['area_m2'] else '',
                       ', '.join(f'{c} {v * 100:.0f}%' for c, v in d['shares'].items()),
                       ', '.join(d['top_materials'])] for d in R['densest_chunks']]))

    out.append('## Slope bands, share of surface area in %\n')
    out.append('Angle of the face normal from straight up; a face on a band edge is in the steeper band. '
               '"Leaning out": normal y in [-0.5, 0); "ceiling": normal y < -0.5.\n')
    rows = [[c, part, *(percent(v) for v in s)] + [num(R['classes'][c][part]['area_m2'])]
            for c, parts in R['slope'].items() for part, s in parts.items() if s is not None]
    out.append(table(['class', 'part', '0-15', '15-30', '30-45', '45-60', '60-90', 'leaning out', 'ceiling', 'area m²'],
                     rows))

    out.append(f'## Surface layers on a {R["cell_m"]} m XZ grid\n')
    out.append(f'Up-facing triangles (normal y > {UP_NORMAL_Y}) above sea level, rasterised at cell centres; a height '
               f'gap over {LAYER_GAP_M} m starts a new layer. Natural = ground + rock. Area = cells × cell².\n')
    rows = []
    for k, L in R['layers'].items():
        n = max(L['cells'], 1)
        g = L['multi_gap_m']
        rows.append([k, f'{L["cells"]:,}', f'{L["one"]:,} ({L["one"] / n * 100:.1f}%)',
                     f'{L["two_or_more"]:,} ({L["two_or_more"] / n * 100:.1f}%)', f'{L["three_or_more"]:,}',
                     f'{g["<=5"]:,} / {g["5-15"]:,} / {g[">15"]:,}'])
    out.append(table(['set', 'cells with a layer', 'one layer', 'two or more', 'three or more',
                      'lowest to highest of multi-layer cells: <=5 / 5-15 / >15 m'], rows))

    o = R['objects']
    out.append('## Objects\n')
    out.append(f'Vobs {o["vobs"]:,} with children; with a mesh or model visual {o["with_mesh_visual"]:,}. '
               'Mesh triangles count the instances whose mesh is in the base (or content) meshes.\n')
    out.append(table(['class', 'instances', 'distinct meshes', 'instances with mesh', 'mesh triangles', 'mean per instance'],
                     [[c, f'{r["instances"]:,}', r['distinct_meshes'], f'{r["instances_with_mesh"]:,}',
                       f'{r["mesh_triangles"]:,}', f'{r["mesh_triangles"] / max(r["instances_with_mesh"], 1):.0f}']
                      for c, r in o['classes'].items()]))
    for c, r in o['classes'].items():
        if r['top']:
            out.append(f'Top meshes, {c}: ' + ', '.join(f'{n} ×{k} ({"n/a" if t is None else t})'
                                                        for n, k, t in r['top'][:6]) + '.\n')
    if o['layers']:
        out.append(table(['vob layer', 'meshes added', 'other added', 'removed', 'moved', 'rejected whole because'],
                         [[L['file'], L['meshes'], L['other_added'], L['removed'], L['moved'],
                           '; '.join(L['rejected'])] for L in o['layers']]))

    out.append('## Texel density, ten largest ground materials\n')
    out.append('Texels per metre = sqrt(UV area × width × height / world area) per triangle over 0.05 m², '
               'with the base .dds size (content textures are not read).\n')
    out.append(table(['material', 'dds', 'area m²', 'median texels/m', 'p10', 'p90'],
                     [[r['material'], r['dds'] or 'n/a', num(r['area_m2']), r.get('median'), r.get('p10'), r.get('p90')]
                      for r in R['texel_density']]))

    out.append('## 40 largest materials by area\n')
    out.append(table(['material', 'group', 'class', 'triangles', 'area m²', 'tris per m²', 'median edge m'],
                     [[m['material'], m['group'], m['class'], f'{m["triangles"]:,}', num(m['area_m2']),
                       f'{m["triangles"] / m["area_m2"]:.3f}' if m['area_m2'] else '', m['median_edge_m']]
                      for m in R['materials']]))
    out.append(f'Census took {R["seconds"]} s.\n')
    return '\n'.join(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('world')
    ap.add_argument('--base')
    ap.add_argument('--content', type=Path)
    where = ap.add_mutually_exclusive_group()
    where.add_argument('--chunks', help='chunk names, e.g. 8_-4,8_-3')
    where.add_argument('--rect', help='X0,Z0,X1,Z1 in chunk coordinates, inclusive')
    ap.add_argument('--cell', type=float, default=1.0, help='grid cell of the layer test, metres (default 1)')
    ap.add_argument('--out', type=Path, help='write <WORLD>_census.md and .json here')
    argv = list(sys.argv[1:] if argv is None else argv)
    for k, arg in enumerate(argv[:-1]):     # '--chunks -2_-3': argparse would take the value for an option
        if arg in ('--chunks', '--rect'):
            argv[k:k + 2] = [f'{arg}={argv[k + 1]}']
            break
    a = ap.parse_args(argv)
    base = resolve_base(a.base)
    if not (base / 'worlds' / a.world / 'world.json').is_file():
        raise SystemExit(f'no world {a.world} in {base}')
    if a.content is not None and not a.content.is_dir():
        raise SystemExit(f'no content folder {a.content}')
    region = parse_chunks(a.chunks) if a.chunks else parse_rect(a.rect) if a.rect else None
    R = census(base, a.world, a.content, region, a.cell)
    text = markdown(R)
    print(text)
    if a.out:
        a.out.mkdir(parents=True, exist_ok=True)
        (a.out / f'{a.world}_census.md').write_text(text)
        (a.out / f'{a.world}_census.json').write_text(json.dumps(R, indent=1, default=float))
    return 0


if __name__ == '__main__':
    sys.exit(main())
