#!/usr/bin/env python3
"""Look at a set's props as the game shows them, before an in-game capture: the asset's cameras, the game's lens and
frame, the texture filtering of the engine, the original beside ours, and the numbers that decide what reads.

uv run --no-project --with numpy --with pillow --with scipy --with OpenEXR python scripts/content/mesh3d/game_view.py \
    --asset city-furniture --set furniture-orig-set-city-12 --out DIR [--views bed-1,bed-2] [--base DIR] \
    [--game-orig CAPTURE_DIR] [--game-set CAPTURE_DIR]

reports/content/2026-11-ai-3d-pipeline/game-view.md: the beds' fabric was judged on texture sheets and close-ups and
failed in the game, where a screen pixel is 5.7 mm of the bed and the engine samples mip 2. Here every camera of the
asset's cameras.json is rendered twice in Blender (EEVEE, blender/game_view.py), the original and the set, each where
its vob stands (vobs.json of the base), with the eye, the target, the 67.5 degree vertical lens and the 1280 x 720
frame of capture-quality-pilot.sh, and 16x anisotropic trilinear filtering as Sampler::anisotrophy(). The light is the
same neutral sky and sun for both, not the engine's: what is compared is ours against the original, not against a
game frame. Per view, inside the prop's mask: the mean luminance and the log-luminance contrast split at 3 cm (broad,
fine), ours over the original; per prop: its size in the vob's axes against the original, its bottom, its triangles
against the budget. With the in-game captures of the original and the set the same numbers are measured on them in
the preview's masks, and the sheet draws the preview's outline over them. DIR gets sheet.jpg, report.json, report.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hearth_targets import BASE_CACHE, ROOT, model_parts  # noqa: E402

FOV_Y = 67.5          # Gothic::options().cameraFov when SystemPack.ini sets none (common/gothic.cpp)
FRAME = (1280, 720)   # TEMPEST_WINDOW_SIZE of capture-quality-pilot.sh
SPLIT_MM = 30.0       # broad and fine contrast part at 3 cm, as hessian_cloth.match_contrast
AMBIENT = 1.0         # scene.ambient over the zenith sky's light (sky.cpp has no closed form for the sky LUT)
LIGHT_MM = 60.0       # the room's light taken from the original's frame varies over this much of the prop
BLENDER = os.environ.get('BLENDER', '/Applications/Blender.app/Contents/MacOS/Blender')
STAGING = Path.home() / '.local/share/gothic-ai/content-staging'
TARGETS = ROOT / 'data/content/mesh3d-furniture.json'


# --- reading meshes ------------------------------------------------------------------------------------------------

def gltf_primitives(path: Path):
    """[(attributes {name: array}, triangles, material dict)] of a .glb, as stored."""
    data = path.read_bytes()
    length = struct.unpack('<I', data[12:16])[0]
    gltf = json.loads(data[20:20 + length])
    blob = data[20 + length + 8:]

    def accessor(index):
        a = gltf['accessors'][index]
        view = gltf['bufferViews'][a['bufferView']]
        dtype = {5126: '<f4', 5123: '<u2', 5125: '<u4', 5121: 'u1'}[a['componentType']]
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        offset = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        return np.frombuffer(blob, dtype, a['count'] * width, offset).reshape(a['count'], width).astype(float)

    for node in gltf.get('nodes', []):
        if 'mesh' in node and any(k in node for k in ('translation', 'rotation', 'matrix')):
            raise SystemExit(f'{path}: a node transform is not supported')
    out = []
    for mesh in gltf['meshes']:
        for prim in mesh['primitives']:
            attributes = {k: accessor(v) for k, v in prim['attributes'].items()
                          if k in ('POSITION', 'NORMAL', 'TEXCOORD_0')}
            out.append((attributes, accessor(prim['indices']).astype(np.int64).reshape(-1, 3),
                        gltf['materials'][prim['material']]))
    return out


def original_parts(base: Path, visual: str):
    """The original's parts [(positions, normals, uv, triangles, texture)], glTF metres of the vob (the X of Gothic
    turned), as hearth_targets measures them."""
    glb = base / 'meshes' / f'{visual}.glb'
    if glb.exists():  # stored in Gothic centimetres
        flip = np.array([-1, 1, 1])
        return [(a['POSITION'] * flip / 100, a['NORMAL'] * flip, a['TEXCOORD_0'], tris,
                 m['extras']['gothic']['texture']) for a, tris, m in gltf_primitives(glb)]
    parts, _ = model_parts(base / 'models', visual, full=True)
    return [(p, n, uv, tris, tex) for p, tris, tex, n, uv in parts]


def own_parts(glb: Path):
    """Ours [(positions, normals, uv, triangles, material name)], already in glTF metres."""
    return [(a['POSITION'], a['NORMAL'], a['TEXCOORD_0'], tris, m['name']) for a, tris, m in gltf_primitives(glb)]


# --- placing them --------------------------------------------------------------------------------------------------

def vob_index(path: Path) -> dict:
    """vob id -> its record, from the base's vobs.json (vobs nest as children)."""
    index, stack = {}, [json.loads(path.read_text())['vobs']]
    while stack:
        item = stack.pop()
        if isinstance(item, list):
            stack.extend(item)
        elif isinstance(item, dict):
            if 'position' in item and 'id' in item:
                index[item['id']] = item
            stack.extend(v for v in item.values() if isinstance(v, (list, dict)))
    return index


def to_world(local_m, vob, transpose=False):
    """glTF metres of a vob (the loader's: X turned, centimetres / 100) -> Gothic world centimetres."""
    rot = np.array(vob['rotation'], dtype=float)
    rot = rot.T if transpose else rot
    return (local_m * np.array([-100, 100, 100])) @ rot.T + np.array(vob['position'], dtype=float)


def placement(parts, vob) -> bool:
    """Whether vobs.json's rotation is stored transposed: the one that lands the original in the vob's bbox."""
    points = np.vstack([p for p, *_ in parts])
    lo, hi = np.array(vob['bbox']['min']), np.array(vob['bbox']['max'])
    miss = []
    for transpose in (False, True):
        w = to_world(points, vob, transpose)
        miss.append(float(np.abs(w.min(0) - lo).max() + np.abs(w.max(0) - hi).max()))
    return miss[1] < miss[0]


def to_blender(world_cm):
    """Gothic world centimetres (left-handed, Y up) -> Blender metres (right-handed, Z up): X turned back, as glTF."""
    w = np.asarray(world_cm, dtype=float) / 100
    return np.stack([-w[..., 0], -w[..., 2], w[..., 1]], axis=-1)


def normals_to_blender(local_n, vob, transpose):
    rot = np.array(vob['rotation'], dtype=float)
    rot = rot.T if transpose else rot
    return to_blender((local_n * np.array([-1, 1, 1])) @ rot.T * 100)


# --- textures ------------------------------------------------------------------------------------------------------

def png(dds: Path, kind: str, cache: Path) -> str:
    """A DDS as the PNG Blender reads (the mip chain is rebuilt by the GPU): albedo RGBA, the DXT5nm normal back to
    RGB (X from alpha, Y from green, turned back to +Y up: pack_materials.py stores V down), ORM as it is."""
    stat = dds.stat()
    key = hashlib.sha1(f'{dds}:{stat.st_size}:{stat.st_mtime_ns}:{kind}'.encode()).hexdigest()[:16]
    out = cache / f'{dds.parent.name}-{dds.stem}-{key}.png'
    if not out.exists():
        im = np.asarray(Image.open(dds).convert('RGBA'), dtype=np.float32) / 255
        if kind == 'normal':
            x, y = im[..., 3] * 2 - 1, 1 - im[..., 1] * 2
            z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
            im = np.dstack([x, y, z]) * 0.5 + 0.5
        elif kind == 'orm':
            im = im[..., :3]
        cache.mkdir(parents=True, exist_ok=True)
        Image.fromarray((np.clip(im, 0, 1) * 255).round().astype(np.uint8)).save(out)
    return str(out)


def original_material(base: Path, texture: str, cache: Path) -> dict:
    dds = base / 'textures' / f'{Path(texture).stem}.dds'
    return {'name': texture, 'albedo': png(dds, 'albedo', cache) if dds.exists() else None}


def own_material(set_dir: Path, base: Path, name: str, cache: Path) -> dict:
    entry = json.loads((set_dir / 'materials/materials.json').read_text()).get(f'{name}.TGA')
    if entry is None:  # a part that keeps an original texture
        return original_material(base, name, cache)
    maps = {k: set_dir / 'materials' / entry[k] for k in ('albedo', 'normal', 'orm') if k in entry}
    return {'name': name, **{k: png(v, k, cache) for k, v in maps.items() if v.exists()}}


# --- the engine's frame --------------------------------------------------------------------------------------------

def exr_layers(path: Path) -> dict:
    """The layers blender/game_view.py renders: col (albedo x (1 - metal)), light (the sky on the shading normal),
    ao (ORM red, 1 for the original), alpha."""
    import OpenEXR
    with OpenEXR.File(str(path)) as f:
        ch = {name: c.pixels for part in f.parts for name, c in part.channels.items()}
    return {'col': ch['ViewLayer.Diffuse Color'][..., :3], 'light': ch['ViewLayer.Diffuse Direct'][..., :3],
            'ao': ch['ViewLayer.ao.X'], 'alpha': ch['ViewLayer.Combined'][..., 3]}


def aces(x):
    """shader/lighting/tonemapping.glsl acesTonemap (Narkowicz)."""
    return np.clip(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0, 1)


def aces_inv(x):
    return (-0.59 * x + 0.03 - np.sqrt(np.maximum(-1.0127 * x * x + 1.3702 * x + 0.0009, 0))) / (2 * (2.43 * x - 2.51))


def engine_frame(layers: dict, ambient: float, exposure: float) -> np.ndarray:
    """The frame as the engine makes it indoors (sun shadowed), sRGB RGBA: textureAlbedo() lifts the albedo
    (inverse ACES of 0.78 x linear, x 5), ambient_light.frag lights it by scene.ambient + 0.8 x sky, ours with the
    sky times its ORM occlusion, then exposure and the ACES tonemap."""
    albedo = aces_inv(layers['col'] * 0.78 + 0.001) * 5
    light = ambient + 0.8 * layers['light'] * layers['ao'][..., None]
    rgb = aces(albedo * light * exposure)
    rgb = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(rgb, 1 / 2.4) - 0.055)
    return np.dstack([rgb, layers['alpha']])


def normalized_blur(values: np.ndarray, mask: np.ndarray, sigma: float) -> np.ndarray:
    """A blur of values inside mask that also fills the pixels around it from the inside."""
    weight = ndimage.gaussian_filter(mask.astype(float), sigma)
    return ndimage.gaussian_filter(np.where(mask, values, 0), sigma) / np.maximum(weight, 1e-6)


def relit_frame(layers: dict, orig: dict, game_orig: np.ndarray, ambient: float, sigma_px: float) -> np.ndarray:
    """Ours under the light the original gets in the game's frame: the lamps, the fire and the sun patches of each
    room, which the preview's sky does not know. The original's frame gives that light (exposure included) where it
    stands, aces_inv(frame) / its lifted albedo, blurred over sigma_px; ours takes it with its own shading under the
    preview's sky, relative to the original's there, so its normal map, occlusion and form still show."""
    mask = orig['alpha'] > 0.5
    inner = ndimage.binary_erosion(mask, iterations=2)
    lum = np.array([0.2126, 0.7152, 0.0722])
    seen = aces_inv(np.clip(linear(game_orig[..., :3]), 0, 0.98))
    albedo_orig = aces_inv(orig['col'] * 0.78 + 0.001) * 5
    field = normalized_blur(seen @ lum, inner, sigma_px) / np.maximum(normalized_blur(albedo_orig @ lum, inner, sigma_px), 1e-6)
    shade_orig = normalized_blur((ambient + 0.8 * orig['light'] * orig['ao'][..., None]) @ lum, inner, sigma_px)
    light = ambient + 0.8 * layers['light'] * layers['ao'][..., None]
    rgb = aces(aces_inv(layers['col'] * 0.78 + 0.001) * 5 * light * (field / np.maximum(shade_orig, 1e-6))[..., None])
    rgb = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(rgb, 1 / 2.4) - 0.055)
    return np.dstack([rgb, layers['alpha']])


def fit_exposure(layers: dict, mask: np.ndarray, game: np.ndarray | None, ambient: float) -> float:
    """The exposure that gives the original the median luminance it has in the game's frame (the original's capture
    is made once per asset), or a mid-grey 0.18 without one."""
    inner = ndimage.binary_erosion(mask, iterations=3)
    if not inner.any():
        return 1.0
    weights = np.array([0.2126, 0.7152, 0.0722])
    want = float(np.median(linear(game[..., :3])[inner] @ weights)) if game is not None else 0.18
    inside = {k: v[inner][:, None] if v.ndim == 2 else v[inner][:, None, :] for k, v in layers.items()}
    lo, hi = -8.0, 8.0
    for _ in range(30):
        mid = (lo + hi) / 2
        got = float(np.median(linear(engine_frame(inside, ambient, 2 ** mid)[..., :3]).reshape(-1, 3) @ weights))
        lo, hi = (mid, hi) if got < want else (lo, mid)
    return 2 ** ((lo + hi) / 2)


# --- numbers -------------------------------------------------------------------------------------------------------

def linear(rgb):
    return np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)


def mm_per_pixel(eye_cm, centre_cm, target_cm) -> float:
    """Millimetres of the prop a screen pixel covers at its centre's depth along the view."""
    forward = np.asarray(target_cm, float) - np.asarray(eye_cm, float)
    depth = float(np.dot(np.asarray(centre_cm, float) - eye_cm, forward / np.linalg.norm(forward)))
    return depth * 10 * 2 * math.tan(math.radians(FOV_Y) / 2) / FRAME[1]


def contrast(rgba: np.ndarray, mask: np.ndarray, mm_px: float) -> dict | None:
    """Mean linear luminance and the log-luminance std of the broad (over 3 cm) and the fine layer inside mask."""
    inner = ndimage.binary_erosion(mask, iterations=3)
    if inner.sum() < 50:
        return None
    lum = linear(rgba[..., :3]) @ np.array([0.2126, 0.7152, 0.0722])
    log = np.log(lum + 1e-3)
    sigma = SPLIT_MM / mm_px / 2.355
    weight = ndimage.gaussian_filter(inner.astype(float), sigma)
    broad = ndimage.gaussian_filter(np.where(inner, log, 0), sigma) / np.maximum(weight, 1e-6)
    fine = log - broad
    return {'mean': round(float(lum[inner].mean()), 5), 'broad': round(float(broad[inner].std()), 4),
            'fine': round(float(fine[inner].std()), 4), 'pixels': int(inner.sum())}


def alignment(frame: np.ndarray, mask: np.ndarray) -> float:
    """How well the preview's outline sits on the game frame: the frame's edge strength on the outline over its mean
    around the prop. About 1 is chance; a view whose hero fell before the shot (bench.log: M_FALL_SMALL) moved the
    game's camera and scores low, and its game numbers mean nothing."""
    grey = frame[..., :3].mean(2)
    edges = np.hypot(ndimage.sobel(grey, 0), ndimage.sobel(grey, 1))
    edge = outline(mask)
    near = ndimage.binary_dilation(mask, iterations=40)
    return round(float(edges[edge].mean() / max(edges[near].mean(), 1e-9)), 2) if edge.any() else 0.0


def ratio(ours: dict | None, orig: dict | None) -> dict | None:
    if not ours or not orig:
        return None
    return {k: round(ours[k] / max(orig[k], 1e-9), 3) for k in ('mean', 'broad', 'fine')}


def checks(orig, ours, budget) -> dict:
    """Size in the vob's axes (glTF metres) ours over the original, the bottom's offset in cm, triangles."""
    po, pn = np.vstack([p for p, *_ in orig]), np.vstack([p for p, *_ in ours])
    so, sn = po.max(0) - po.min(0), pn.max(0) - pn.min(0)
    tris = int(sum(len(t) for *_, t, _ in ours))
    return {'size': [round(float(a / max(b, 1e-9)), 3) for a, b in zip(sn, so)],
            'bottom_cm': round(float(pn[:, 1].min() - po[:, 1].min()) * 100, 1),
            'tris': tris, 'budget': budget, 'over_budget': bool(budget and tris > budget)}


# --- the run -------------------------------------------------------------------------------------------------------

def newest_base() -> Path:
    bases = [p for p in BASE_CACHE.iterdir() if (p / 'worlds/NEWWORLD/vobs.json').exists()]
    return max(bases, key=lambda p: (p / 'worlds/NEWWORLD/vobs.json').stat().st_mtime)


def write_object(path: Path, parts, vob, transpose, materials) -> dict:
    p = np.vstack([to_blender(to_world(q, vob, transpose)) for q, *_ in parts])
    n = np.vstack([normals_to_blender(q, vob, transpose) for _, q, *_ in parts])
    uv = np.vstack([q for _, _, q, *_ in parts])
    offsets = np.cumsum([0] + [len(q) for q, *_ in parts])
    tris = np.vstack([t + o for (*_, t, _), o in zip(parts, offsets)])
    slots = np.concatenate([np.full(len(t), i) for i, (*_, t, _) in enumerate(parts)])
    np.savez(path, positions=p, normals=n, uv=uv, triangles=tris, slots=slots)
    return {'npz': str(path), 'materials': materials}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--asset', required=True)
    ap.add_argument('--set', required=True, help='a content-staging set name or folder')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--views', help='camera names, comma separated (default: all)')
    ap.add_argument('--base', type=Path)
    ap.add_argument('--game-orig', type=Path, help='in-game captures of the original (<view>.png)')
    ap.add_argument('--game-set', type=Path, help='in-game captures of the same set')
    ap.add_argument('--ambient', type=float, default=AMBIENT, help='scene.ambient over the zenith sky')
    ap.add_argument('--reuse', action='store_true', help='keep the renders in DIR/work, only measure again')
    a = ap.parse_args()
    started = time.time()
    base = a.base or newest_base()
    set_dir = Path(a.set) if Path(a.set).is_dir() else STAGING / a.set
    cameras = json.loads((ROOT / 'reports/content/remaster' / a.asset / 'cameras.json').read_text())['cameras']
    if a.views:
        wanted = a.views.split(',')
        cameras = [c for c in cameras if c['name'] in wanted]
    vobs = vob_index(base / 'worlds/NEWWORLD/vobs.json')
    meshes = json.loads((set_dir / 'meshes/meshes.json').read_text())
    budgets = {t['visual']: t['budget_tris'] for t in json.loads(TARGETS.read_text())['targets']} \
        if TARGETS.exists() else {}
    a.out.mkdir(parents=True, exist_ok=True)
    work, cache = a.out / 'work', a.out / 'textures'
    work.mkdir(exist_ok=True)

    views, props = [], {}
    for cam in cameras:
        found = re.search(r'vob (\d+)', cam.get('note', ''))
        if not found:
            print('skip', cam['name'], '(no vob in its note)')
            continue
        vob = vobs[int(found.group(1))]
        visual = Path(vob['visual_name']).stem.upper()
        orig = original_parts(base, visual)
        transpose = placement(orig, vob)
        view = {'name': cam['name'], 'visual': visual, 'vob': vob['id'],
                'eye': to_blender(cam['eye']).tolist(), 'target': to_blender(cam['target']).tolist(),
                'orig': write_object(work / f"{cam['name']}-orig.npz", orig, vob, transpose,
                                     [original_material(base, tex, cache) for *_, tex in orig])}
        centre = np.vstack([to_world(p, vob, transpose) for p, *_ in orig])
        view['mm_px'] = round(mm_per_pixel(cam['eye'], (centre.min(0) + centre.max(0)) / 2, cam['target']), 3)
        if visual in meshes:
            ours = own_parts(set_dir / 'meshes' / meshes[visual]['glb'])
            view['ours'] = write_object(work / f"{cam['name']}-ours.npz", ours, vob, transpose,
                                        [own_material(set_dir, base, name, cache) for *_, name in ours])
            props.setdefault(visual, checks(orig, ours, budgets.get(visual)))
        views.append(view)

    scene = {'fov_y': FOV_Y, 'frame': FRAME, 'out': str(work), 'views': views}
    (work / 'scene.json').write_text(json.dumps(scene, indent=1))
    if not a.reuse:
        run = subprocess.run([BLENDER, '-b', '--factory-startup', '-P',
                              str(Path(__file__).resolve().parent / 'blender/game_view.py'), '--',
                              str(work / 'scene.json')], capture_output=True, text=True)
        if run.returncode or 'GAME_VIEW DONE' not in run.stdout:
            print(run.stdout[-3000:], run.stderr[-3000:])
            raise SystemExit('blender failed')

    report = {'asset': a.asset, 'set': set_dir.name, 'base': base.name, 'props': props, 'views': []}
    rows = []
    for view in views:
        name = view['name']
        passes = {k: exr_layers(work / f'{name}-{k}.exr') for k in ('orig', 'ours') if (work / f'{name}-{k}.exr').exists()}
        masks = {k: v['alpha'] > 0.5 for k, v in passes.items()}
        game = {}
        for key, folder in (('orig', a.game_orig), ('ours', a.game_set)):
            frame = folder / f'{name}.png' if folder else None
            if frame and frame.exists() and key in masks:
                game[key] = np.asarray(Image.open(frame).convert('RGBA'), dtype=np.float32) / 255
                game[key][..., 3] = 1
        exposure = fit_exposure(passes['orig'], masks['orig'], game.get('orig'), a.ambient)
        if 'orig' in game:  # the room's own light, from the original's frame
            sigma = LIGHT_MM / view['mm_px'] / 2.355
            shots = {k: relit_frame(v, passes['orig'], game['orig'], a.ambient, sigma) for k, v in passes.items()}
        else:
            shots = {k: engine_frame(v, a.ambient, exposure) for k, v in passes.items()}
        measured = {k: contrast(v, masks[k], view['mm_px']) for k, v in shots.items()}
        game_measured = {k: contrast(v, masks[k], view['mm_px']) for k, v in game.items()}
        entry = {'view': name, 'visual': view['visual'], 'mm_px': view['mm_px'], 'exposure': round(exposure, 4),
                 'preview': measured, 'preview_ratio': ratio(measured.get('ours'), measured.get('orig'))}
        if game_measured:
            entry['align'] = {k: alignment(v, masks[k]) for k, v in game.items()}
            entry['game'] = game_measured
            entry['game_ratio'] = ratio(game_measured.get('ours'), game_measured.get('orig'))
        report['views'].append(entry)
        rows.append((name, shots, masks, game))
    report['seconds'] = round(time.time() - started, 1)
    (a.out / 'report.json').write_text(json.dumps(report, indent=1, ensure_ascii=False))
    (a.out / 'report.md').write_text(markdown(report))
    sheet(rows, a.out / 'sheet.jpg')
    print(markdown(report))
    return 0


# --- what the user sees --------------------------------------------------------------------------------------------

def markdown(report: dict) -> str:
    lines = [f"# game_view: {report['asset']} / {report['set']}", '',
             '| вид | мм/пикс | яркость | крупный | мелкий | в игре: яркость | крупный | мелкий |', '|---|---|---|---|---|---|---|---|']
    for v in report['views']:
        r, g = v.get('preview_ratio') or {}, v.get('game_ratio') or {}
        cells = [r.get(k, '—') for k in ('mean', 'broad', 'fine')] + [g.get(k, '—') for k in ('mean', 'broad', 'fine')]
        lines.append(f"| {v['view']} | {v['mm_px']} | " + ' | '.join(str(c) for c in cells) + ' |')
    lines += ['', 'Отношения «наше / оригинал» в маске предмета; контраст — std log-яркости, крупный и мелкий слой по 3 см.',
              '', '| предмет | размер x/y/z | низ, см | треугольники | бюджет |', '|---|---|---|---|---|']
    for visual, c in report['props'].items():
        flag = ' ⚠' if c['over_budget'] or any(abs(s - 1) > 0.05 for s in c['size']) or abs(c['bottom_cm']) > 2 else ''
        lines.append(f"| {visual}{flag} | {' / '.join(map(str, c['size']))} | {c['bottom_cm']} | {c['tris']} | {c['budget']} |")
    lines += ['', f"{len(report['views'])} видов за {report['seconds']} с."]
    return '\n'.join(lines) + '\n'


def outline(mask: np.ndarray) -> np.ndarray:
    return mask & ~ndimage.binary_erosion(mask, iterations=2)


def sheet(rows, path: Path, height: int = 300):
    """Per view: the game's original with the preview's outline | preview original | preview ours | the game's set."""
    tiles = []
    for name, shots, masks, game in rows:
        union = np.zeros(FRAME[::-1], bool)
        for m in masks.values():
            union |= m
        ys, xs = np.nonzero(union) if union.any() else (np.array([0, FRAME[1] - 1]), np.array([0, FRAME[0] - 1]))
        pad = 30
        box = (max(xs.min() - pad, 0), max(ys.min() - pad, 0), min(xs.max() + pad, FRAME[0]), min(ys.max() + pad, FRAME[1]))
        cells = []
        for key, src in (('game-orig', game.get('orig')), ('orig', shots.get('orig')), ('ours', shots.get('ours')),
                         ('game-set', game.get('ours'))):
            if src is None:
                continue
            rgb = src[..., :3] * src[..., 3:] + 0.35 * (1 - src[..., 3:])  # the preview on mid grey
            if key.startswith('game'):
                edge = outline(masks['orig' if key == 'game-orig' else 'ours'])
                rgb = np.where(edge[..., None], np.array([1.0, 0.2, 0.1]), rgb)
            im = Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).crop(box)
            im = im.resize((round(im.width * height / im.height), height), Image.LANCZOS)
            ImageDraw.Draw(im).text((6, 4), f'{name} {key}', fill=(255, 255, 255))
            cells.append(im)
        row = Image.new('RGB', (sum(c.width for c in cells) + 4 * len(cells), height), (20, 20, 20))
        x = 0
        for c in cells:
            row.paste(c, (x, 0))
            x += c.width + 4
        tiles.append(row)
    width = max(t.width for t in tiles)
    out = Image.new('RGB', (width, sum(t.height + 4 for t in tiles)), (20, 20, 20))
    y = 0
    for t in tiles:
        out.paste(t, (0, y))
        y += t.height + 4
    out.save(path, quality=88)


if __name__ == '__main__':
    raise SystemExit(main())
