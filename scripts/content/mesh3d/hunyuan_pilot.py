#!/usr/bin/env python3
"""The Hunyuan pilot on the Mac side (reports/content/2026-11-ai-3d-pipeline/hunyuan-pilot.md): the original's controls for
Hunyuan3D-Omni, the numbers, the sheet.

uv run --no-project --with numpy --with scipy --with pillow --with trimesh python scripts/content/mesh3d/hunyuan_pilot.py \
    cond|measure|sheet PILOT_DIR

PILOT_DIR (outside git: the Hunyuan licences forbid showing their output in the EU, the UK and South Korea) holds
out21/, omni-point/, omni-bbox/ (hunyuan_run.py's .glb, fetched from dev-ubuntu).
cond: for each item the original's mesh (hearth_targets: base meshes/ or a mob model's bind pose) turned by the yaw that
best fits Hunyuan3D-2.1's shape of the same picture (2.1 builds a 3/4 picture in a frame of its own), as 2048 surface
points in the cube Omni's inference.py normalises to (cond/<item>.ply) and as its extents (cond/<item>.json).
measure: each generator's shape (TRELLIS.2: gen/<item>/high.npz; 2.1; Omni) turned to the original the same way,
scaled as one: its proportions against the original's (per axis, the largest extent 1), the chamfer distance to it in
the same unit, and its mesh: boundary and non-manifold edges, pieces. Writes PILOT_DIR/measure.json.
sheet: renders of the original and each shape, side by side (blender/parts_sheet.py's way), PILOT_DIR/sheet.jpg.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import trimesh
from scipy.spatial import cKDTree

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hearth_targets import BASE_CACHE, ROOT, glb_parts, model_parts  # noqa: E402

GEN = Path.home() / '.local/share/gothic-ai/content-staging/city-furniture/gen'
TARGETS = ROOT / 'data/content/mesh3d-furniture.json'
BLENDER = '/Applications/Blender.app/Contents/MacOS/Blender'
SHAPES = ['trellis2', 'out21', 'omni-point', 'omni-bbox']


def newest_base() -> Path:
    return max((p for p in BASE_CACHE.iterdir() if (p / 'meshes').is_dir()), key=lambda p: p.stat().st_mtime)


def visual_of(item: str) -> str:
    cid = json.loads((GEN / item / 'config.json').read_text())['id']
    return next(t['visual'] for t in json.loads(TARGETS.read_text())['targets'] if t['id'] == cid)


def original(item: str) -> trimesh.Trimesh:
    """The original in glTF metres (Y up)."""
    base, visual = newest_base(), visual_of(item)
    glb = base / 'meshes' / f'{visual}.glb'
    parts = glb_parts(glb) if glb.exists() else model_parts(base / 'models', visual)[0]
    offsets = np.cumsum([0] + [len(p) for p, _, _ in parts])
    return trimesh.Trimesh(np.vstack([p for p, _, _ in parts]),
                           np.vstack([t + o for (_, t, _), o in zip(parts, offsets)]), process=False)


def shape(pilot: Path, kind: str, item: str) -> trimesh.Trimesh | None:
    if kind == 'trellis2':
        d = np.load(GEN / item / 'high.npz')
        v = d['vertices']
        return trimesh.Trimesh(np.stack([v[:, 0], v[:, 2], -v[:, 1]], axis=1), d['faces'], process=False)  # Z up -> Y up
    path = pilot / kind / f'{item}.glb'
    return trimesh.load(path, force='mesh') if path.exists() else None


def unit(points: np.ndarray) -> np.ndarray:
    """Centred on the bounding box, the largest extent 1."""
    lo, hi = points.min(0), points.max(0)
    return (points - (lo + hi) / 2) / (hi - lo).max()


def yaw(points: np.ndarray, quarter: int, mirror: bool = False) -> np.ndarray:
    p = points * np.array([-1, 1, 1]) if mirror else points
    for _ in range(quarter):
        p = np.stack([p[:, 2], p[:, 1], -p[:, 0]], axis=1)
    return p


def chamfer(a: np.ndarray, b: np.ndarray) -> float:
    return float(cKDTree(b).query(a)[0].mean() + cKDTree(a).query(b)[0].mean()) / 2


def best_turn(source: np.ndarray, target: np.ndarray, mirror: bool = True):
    """The quarter turn about Y (and, if allowed, a mirror) that best lays source on target, both as unit clouds."""
    t = unit(target)
    options = [(chamfer(unit(yaw(source, q, m)), t), q, m) for q in range(4) for m in ((False, True) if mirror else (False,))]
    return min(options)


def surface(mesh: trimesh.Trimesh, n: int = 20000, seed: int = 0) -> np.ndarray:
    return trimesh.sample.sample_surface(mesh, n, seed=seed)[0]


def cond(pilot: Path):
    out = pilot / 'cond'
    out.mkdir(exist_ok=True)
    for path in sorted((pilot / 'out21').glob('*.glb')):
        item = path.stem
        orig = surface(original(item), 2048)
        hy = surface(trimesh.load(path, force='mesh'))
        score, q, m = best_turn(orig, hy, mirror=False)  # the control must be the original itself, only turned
        p = yaw(orig, q, m)
        lo, hi = p.min(0), p.max(0)
        points = (p - (lo + hi) / 2) / (hi - lo).max() * 2 * 0.98  # inference.normalize_mesh(scale=0.98)
        trimesh.PointCloud(points).export(out / f'{item}.ply')
        (out / f'{item}.json').write_text(json.dumps({'bbox': ((hi - lo) / (hi - lo).max()).round(4).tolist()}))
        print(item, 'turn', q, 'mirror', m, 'fit', round(score, 4))


def mesh_health(mesh: trimesh.Trimesh) -> dict:
    """Edges and pieces of the welded mesh on integer keys: trimesh's processed copy, unique rows and split of a
    2-3M face shape ran the Mac out of memory."""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    v, f = np.asarray(mesh.vertices), np.asarray(mesh.faces)
    q = np.round((v - v.min(0)) / (np.ptp(v, 0).max() * 1e-6)).astype(np.int64)  # weld within a millionth of the size
    _, weld = np.unique((q[:, 0] << 42) | (q[:, 1] << 21) | q[:, 2], return_inverse=True)
    f = weld.ravel()[f]
    f = f[(f[:, 0] != f[:, 1]) & (f[:, 1] != f[:, 2]) & (f[:, 2] != f[:, 0])]
    e = np.sort(np.concatenate([f[:, [0, 1]], f[:, [1, 2]], f[:, [2, 0]]]), axis=1)
    n = int(weld.max()) + 1
    _, counts = np.unique(e[:, 0] * n + e[:, 1], return_counts=True)
    graph = coo_matrix((np.ones(len(e), np.int8), (e[:, 0], e[:, 1])), shape=(n, n))
    labels = connected_components(graph, directed=False)[1]
    return {'faces': int(len(f)), 'boundary_edges': int((counts == 1).sum()),
            'nonmanifold_edges': int((counts > 2).sum()), 'pieces': int(len(np.unique(labels[np.unique(f)])))}


def measure(pilot: Path):
    report = {}
    for path in sorted((pilot / 'out21').glob('*.glb')):
        item = path.stem
        orig = surface(original(item))
        oext = orig.max(0) - orig.min(0)
        row = {}
        for kind in SHAPES:
            mesh = shape(pilot, kind, item)
            if mesh is None:
                continue
            pts = surface(mesh)
            fit, q, m = best_turn(pts, orig)
            turned = yaw(pts, q, m)
            ext = turned.max(0) - turned.min(0)
            row[kind] = {'proportion': ((ext / ext.max()) / (oext / oext.max())).round(3).tolist(),
                         'chamfer': round(fit, 4), **mesh_health(mesh)}
        report[item] = row
        print(item, {k: (v['proportion'], v['chamfer']) for k, v in row.items()})
    (pilot / 'measure.json').write_text(json.dumps(report, indent=1))


def sheet(pilot: Path):
    """Each shape turned onto the original, as one unit cloud, rendered by blender/hunyuan_sheet.py."""
    from PIL import Image, ImageDraw
    work = pilot / 'sheet-work'
    work.mkdir(exist_ok=True)
    items = sorted(p.stem for p in (pilot / 'out21').glob('*.glb'))
    for item in items:
        orig = original(item)
        trimesh.Trimesh(unit(orig.vertices), orig.faces).export(work / f'{item}-original.ply')
        target = surface(orig)
        for kind in SHAPES:
            mesh = shape(pilot, kind, item)
            if mesh is None:
                continue
            _, q, m = best_turn(surface(mesh), target)
            faces = mesh.faces[:, ::-1] if m else mesh.faces
            trimesh.Trimesh(unit(yaw(np.asarray(mesh.vertices), q, m)), faces).export(work / f'{item}-{kind}.ply')
    subprocess.run([BLENDER, '-b', '--factory-startup', '-P', str(Path(__file__).resolve().parent / 'blender/hunyuan_sheet.py'),
                    '--', str(work)], check=True, capture_output=True)
    columns = ['original'] + SHAPES
    cell = 300
    image = Image.new('RGB', (cell * len(columns), cell * len(items)), (30, 30, 30))
    for r, item in enumerate(items):
        for c, kind in enumerate(columns):
            png = work / f'{item}-{kind}.png'
            if png.exists():
                tile = Image.open(png).convert('RGB').resize((cell, cell))
                ImageDraw.Draw(tile).text((6, 4), f'{item} {kind}', fill=(255, 255, 255))
                image.paste(tile, (c * cell, r * cell))
    image.save(pilot / 'sheet.jpg', quality=88)
    print(pilot / 'sheet.jpg')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('command', choices=['cond', 'measure', 'sheet'])
    ap.add_argument('pilot', type=Path)
    a = ap.parse_args()
    {'cond': cond, 'measure': measure, 'sheet': sheet}[a.command](a.pilot)


if __name__ == '__main__':
    main()
