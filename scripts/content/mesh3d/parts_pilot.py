#!/usr/bin/env python3
"""The parts pilot (reports/content/2026-11-ai-3d-pipeline/parts-pilot.md): can a model find the generator mesh's cloth,
which the own.cut boxes of gen.json now mark by hand?

uv run --no-project --with numpy --with scipy --with pillow --with trimesh python scripts/content/mesh3d/parts_pilot.py prepare OUT
uv run --no-project --with numpy --with scipy --with pillow --with trimesh python scripts/content/mesh3d/parts_pilot.py score OUT

prepare: for each item of ITEMS the game mesh (low.npz) and the dense coloured one (high.npz) placed as gen_prop.py
places them (its yaw, then each axis fitted into the original's bounding box; Blender axes), written as
OUT/<item>/low.ply and high.ply in glTF axes, and OUT/<item>/truth.npz: per game face its centre, area, "cloth" (the
cut boxes take it, as gen_prop.cut) and "iron" (furniture_gen.zone_mask on the dense mesh's colour nearest to it).
sheet: the three beds' labellings side by side, OUT/sheet.jpg.
score: OUT/<item>/p3sam/parts.npz (P3-SAM's parts on its cleaned copy of low.ply, matched back by face centres)
and OUT/<item>/sam3.npz (labels per game face) against the truth; writes OUT/score.json and prints a table.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

GEN = Path.home() / '.local/share/gothic-ai/content-staging/city-furniture/gen'
ROOT = Path(__file__).resolve().parents[3]
BLENDER = '/Applications/Blender.app/Contents/MacOS/Blender'
ITEMS = ['bed-o', 'bed-rich-o', 'bed-master-o', 'cupboard-rich-1-o', 'chair-rich-o']

sys.path.insert(0, str(Path(__file__).resolve().parent))
from furniture_gen import zone_mask  # noqa: E402


def rotation(cfg: dict) -> np.ndarray:
    """gen_prop.py's turn of the generator's frame: up, tilt, then yaw."""
    yaw, tilt = math.radians(cfg.get('yaw_deg', 0)), math.radians(cfg.get('tilt_deg', 0))
    rot = np.array([[math.cos(yaw), -math.sin(yaw), 0], [math.sin(yaw), math.cos(yaw), 0], [0, 0, 1]])
    rot = rot @ np.array([[1, 0, 0], [0, math.cos(tilt), -math.sin(tilt)], [0, math.sin(tilt), math.cos(tilt)]])
    if cfg.get('up') == 'y':
        rot = rot @ np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]])
    return rot


def goal_box(cfg: dict):
    targets = json.loads(Path(cfg['target']).read_text())['targets']
    goal = next(t for t in targets if t['id'] == cfg['id'])
    gmin, gmax = np.array(goal['bbox_min']), np.array(goal['bbox_max'])
    return np.array([gmin[0], -gmax[2], gmin[1]]), np.array([gmax[0], -gmin[2], gmax[1]])


def glb_axes(points):
    """gen_prop.glb_centres: glTF axes of Blender points, and |z|."""
    return {'x': points[:, 0], 'y': points[:, 2], 'z': -points[:, 1], 'az': np.abs(points[:, 1])}


def in_boxes(g, boxes):
    hit = np.zeros(g['x'].shape, bool)
    for box in boxes:
        inside = np.ones(g['x'].shape, bool)
        for key, span in box.items():
            if key != 'any':
                inside &= (g[key] >= span[0]) & (g[key] <= span[1])
        hit |= inside
    return hit


def cut_faces(V, F, boxes):
    """gen_prop.cut's choice: a face whose centre is in a box, or with any vertex in an "any" box."""
    drop = in_boxes(glb_axes(V[F].mean(1)), [b for b in boxes if not b.get('any')])
    loose = [b for b in boxes if b.get('any')]
    if loose:
        drop |= in_boxes(glb_axes(V), loose)[F].any(axis=1)
    return drop


def write_ply(path: Path, V, F, colour=None):
    """Binary PLY in glTF axes (x, z_blender, -y_blender)."""
    G = np.stack([V[:, 0], V[:, 2], -V[:, 1]], axis=1).astype('<f4')
    head = ['ply', 'format binary_little_endian 1.0', f'element vertex {len(G)}',
            'property float x', 'property float y', 'property float z']
    if colour is not None:
        head += ['property uchar red', 'property uchar green', 'property uchar blue']
    head += [f'element face {len(F)}', 'property list uchar int vertex_indices', 'end_header']
    with path.open('wb') as f:
        f.write(('\n'.join(head) + '\n').encode())
        if colour is None:
            f.write(G.tobytes())
        else:
            rec = np.zeros(len(G), dtype=[('p', '<f4', 3), ('c', 'u1', 3)])
            rec['p'], rec['c'] = G, colour
            f.write(rec.tobytes())
        rec = np.zeros(len(F), dtype=[('n', 'u1'), ('i', '<i4', 3)])
        rec['n'], rec['i'] = 3, F
        f.write(rec.tobytes())


def prepare(out: Path):
    for item in ITEMS:
        folder = GEN / item
        cfg = json.loads((folder / 'config.json').read_text())
        rot = rotation(cfg)
        glo, ghi = goal_box(cfg)
        high = np.load(cfg['raw'])
        HV = high['vertices'].astype(np.float64) @ rot.T
        cmin, cmax = HV.min(0), HV.max(0)  # gen_prop drops floaters first; cumesh_clean left none to matter
        scale = (ghi - glo) / np.maximum(cmax - cmin, 1e-6)
        HV = (HV - cmin) * scale + glo
        low = np.load(cfg['low'])
        LV = (low['vertices'].astype(np.float64) @ rot.T - cmin) * scale + glo
        LF = low['faces'].astype(np.int64)
        centres = LV[LF].mean(1)
        area = 0.5 * np.linalg.norm(np.cross(LV[LF[:, 1]] - LV[LF[:, 0]], LV[LF[:, 2]] - LV[LF[:, 0]]), axis=1)
        cloth = cut_faces(LV, LF, cfg['own']['cut']) if cfg.get('own', {}).get('cut') else np.zeros(len(LF), bool)
        linear = np.clip(high['color'], 0, 1)
        srgb = np.where(linear <= 0.0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - 0.055)
        _, near = cKDTree(HV).query(centres)
        iron = zone_mask(srgb[near][None], 'iron')[0] > 0.5
        target = out / item
        target.mkdir(parents=True, exist_ok=True)
        write_ply(target / 'low.ply', LV, LF)
        keep = np.random.default_rng(0).random(len(high['faces'])) < 1.0
        write_ply(target / 'high.ply', HV, high['faces'][keep], (srgb * 255).round().astype(np.uint8))
        np.savez(target / 'truth.npz', centres=centres, area=area, cloth=cloth, iron=iron, vertices=LV, faces=LF)
        print(item, 'faces', len(LF), 'cloth', round(float(area[cloth].sum() / area.sum()), 3),
              'iron', round(float(area[iron].sum() / area.sum()), 3))


def iou(pred, truth, area):
    inter, union = area[pred & truth].sum(), area[pred | truth].sum()
    return {'iou': round(float(inter / max(union, 1e-12)), 3),
            'extra': round(float(area[pred & ~truth].sum() / area.sum()), 4),
            'missed': round(float(area[~pred & truth].sum() / area.sum()), 4)}


def oracle(parts, truth, area):
    """The best a person could do by picking whole parts: a part is taken when most of its area is the truth."""
    pick, chosen = np.zeros(len(parts), bool), []
    for p in np.unique(parts):
        m = parts == p
        if area[m & truth].sum() > 0.5 * area[m].sum():
            pick |= m
            chosen.append(int(p))
    return pick, chosen


def p3sam_labels(folder: Path, centres):
    """P3-SAM's part per game face: the faces of its cleaned copy (glTF axes, run_p3sam.py's parts.npz), matched back
    by the nearest face centre."""
    path = folder / 'p3sam' / 'parts.npz'
    if not path.exists():
        return None
    d = np.load(path)
    c = d['vertices'][d['faces']].mean(1)
    _, near = cKDTree(np.stack([c[:, 0], -c[:, 2], c[:, 1]], axis=1)).query(centres)
    return d['face_ids'][near]


def score(out: Path):
    table = {}
    for item in ITEMS:
        folder = out / item
        t = np.load(folder / 'truth.npz')
        area, centres = t['area'], t['centres']
        row = {}
        for kind in ('cloth', 'iron'):
            truth = t[kind]
            if not truth.any():
                continue
            parts = p3sam_labels(folder, centres)
            if parts is not None:
                pick, chosen = oracle(parts, truth, area)
                row[f'p3sam_{kind}'] = {**iou(pick, truth, area), 'parts': len(np.unique(parts)), 'picked': len(chosen)}
            sam = folder / 'sam3.npz'
            if sam.exists() and kind in np.load(sam).files:
                row[f'sam3_{kind}'] = iou(np.load(sam)[kind], truth, area)
        table[item] = row
    (out / 'score.json').write_text(json.dumps(table, indent=1))
    for item, row in table.items():
        for key, r in row.items():
            print(f'{item:20s} {key:12s} ' + ' '.join(f'{k} {v}' for k, v in r.items()))


def sheet(out: Path):
    """Columns per bed: the hand cut, P3-SAM's best pick, all its parts, SAM 3; rendered by blender/parts_sheet.py."""
    import subprocess
    from PIL import Image, ImageDraw
    rows = []
    for item in ITEMS[:3]:
        folder = out / item
        t = np.load(folder / 'truth.npz')
        columns = {'cut': t['cloth'].astype(int)}
        parts = p3sam_labels(folder, t['centres'])
        if parts is not None:
            columns['p3sam-pick'] = oracle(parts, t['cloth'], t['area'])[0].astype(int)
            columns['p3sam-parts'] = parts.astype(int) + 2
        if (folder / 'sam3.npz').exists():
            columns['sam3'] = np.load(folder / 'sam3.npz')['cloth'].astype(int)
        np.savez(folder / 'labels.npz', **columns)
        rows.append((item, list(columns)))
    renders = out / 'sheet'
    subprocess.run([BLENDER, '-b', '--factory-startup', '-P', str(Path(__file__).resolve().parent / 'blender/parts_sheet.py'),
                    '--', str(out), str(renders)], check=True, capture_output=True)
    cell = 420
    width = max(len(c) for _, c in rows) * 2 * cell
    image = Image.new('RGB', (width, len(rows) * cell), (30, 30, 30))
    for r, (item, columns) in enumerate(rows):
        for c, column in enumerate(columns):
            for k in range(2):
                tile = Image.open(renders / f'{item}-{column}-{k}.png').convert('RGB')
                ImageDraw.Draw(tile).text((8, 6), f'{item} {column}', fill=(255, 255, 255))
                image.paste(tile, ((c * 2 + k) * cell, r * cell))
    image.save(out / 'sheet.jpg', quality=88)
    print(out / 'sheet.jpg')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('command', choices=['prepare', 'score', 'sheet'])
    ap.add_argument('out', type=Path)
    a = ap.parse_args()
    {'prepare': prepare, 'score': score, 'sheet': sheet}[a.command](a.out)


if __name__ == '__main__':
    main()
