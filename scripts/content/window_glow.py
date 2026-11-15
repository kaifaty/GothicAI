#!/usr/bin/env python3
"""Glow masks of windows and a pilot content folder for them (reports/render-features/2026-11-15-night-interiors).

    uv run --no-project --with numpy --with pillow python scripts/content/window_glow.py pilot \
        --content ~/.local/share/gothic-ai/content --output <new folder> [--sheet masks.png]
    uv run --no-project --with numpy --with pillow python scripts/content/window_glow.py clip <run of \
        window-glow-dusk.bench> <out.mp4>

A window lit of an evening is a material with `emissive` and `window` in materials.json whose ORM alpha marks what
glows (OpenGothic/common/graphics/windowglow.h). `pilot` clones a content folder (APFS clones where the disk can),
rewrites the ORM of the window textures below as DXT5 with that mask in the alpha and adds the two fields to their
records. The source folder is not written to. The mask comes from the albedo: light gets out where the picture
shows a gap - between the leaves of closed shutters, around them, between their planks - or an opening.
The ORM's colour channels are decoded and compressed again; a set built from its source maps should write them
from those instead.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]


def gauss(distance: np.ndarray, sigma: float) -> np.ndarray:
    return np.exp(-0.5 * (distance / sigma) ** 2)


def stretches(length: int, seed: int, open_share: float, period: float) -> np.ndarray:
    """0..1 along a gap: it leaks over about open_share of its length, in stretches of about `period` texels."""
    rng = np.random.default_rng(seed)
    knots = rng.random(int(length / period) + 3)
    at = np.arange(length) / period
    i = at.astype(int)
    f = at - i
    f = f * f * (3.0 - 2.0 * f)
    smooth = knots[i] * (1.0 - f) + knots[i + 1] * f
    return np.clip((smooth - (1.0 - open_share)) / 0.18 + 0.5, 0.0, 1.0)


def crack(luma: np.ndarray, x: float, y0: int, y1: int, strength: float, sigma: float, search: int = 4,
          tight: float = 0.0, leaks: tuple[int, float] | None = None) -> np.ndarray:
    """A vertical gap near column x between rows y0 and y1: it follows the darkest texel of each row and lets light
    through where the picture is dark there (an open gap), not where the planks meet tight. tight: how much darker
    than its surroundings (0..1) the gap must be before any light gets through - planks that meet leak in places.
    leaks: (seed, share) - the gap is shut over the rest of its length, as a leaf lies against its frame."""
    h, w = luma.shape
    out = np.zeros_like(luma)
    xs = np.arange(w, dtype=np.float32)
    centre = float(x)
    for y in range(y0, y1):
        lo, hi = round(centre) - search, round(centre) + search + 1
        at = lo + int(np.argmin(luma[y, lo:hi]))
        centre = 0.8 * centre + 0.2 * at  # the gap wanders slowly, a knot does not pull it aside
        centre = min(max(centre, x - search), x + search)
        around = float(np.median(luma[y, max(lo - 12, 0):hi + 12]))
        open_ = np.clip((around - luma[y, at]) / max(around, 1e-3), 0.0, 1.0)
        open_ = np.clip((open_ - tight) / max(1.0 - tight, 1e-3), 0.0, 1.0)
        out[y] = strength * open_ * gauss(xs - at, sigma)
    # the ends of the gap fade over a few texels
    fade = np.clip(np.minimum(np.arange(h) - y0, y1 - 1 - np.arange(h)) / 6.0, 0.0, 1.0)
    if leaks is not None:
        along = np.zeros(h, dtype=np.float32)
        along[y0:y1] = stretches(y1 - y0, leaks[0], leaks[1], 46.0 * h / 1024.0)
        fade = fade * along
    return out * fade[:, None]


def shutters(albedo: np.ndarray) -> np.ndarray:
    """NW_CITY_WINDOW_03: two closed leaves of planks in a timber frame. In texels of a 1024 map: the leaves fill
    x 240..772, y 184..647, they meet at x 499."""
    s = albedo.shape[0] / 1024.0
    luma = albedo.mean(axis=2)
    def px(v: float) -> int:
        return round(v * s)

    mask = np.zeros_like(luma)
    y0, y1 = px(190), px(642)
    # between the leaves: the widest gap
    mask = np.maximum(mask, crack(luma, 499.5 * s, y0, y1, 1.0, 1.5 * s, px(4)))
    # between the planks: only where a joint has opened
    for x in (275, 344, 402, 456, 550, 587, 662, 710):
        mask = np.maximum(mask, crack(luma, x * s, y0 + px(6), y1 - px(6), 0.35, 0.9 * s, px(4), tight=0.72))
    # around the leaves, against the frame: the hinge sides are tight, the top and the sill less so
    for x, k, seed in ((240.5, 0.25, 11), (771.5, 0.25, 12)):
        mask = np.maximum(mask, crack(luma, x * s, y0, y1, k, 1.0 * s, px(3), tight=0.8, leaks=(seed, 0.4)))
    turned = luma.T.copy()
    for y, k, tight, seed, share in ((184.5, 0.3, 0.75, 13, 0.45), (646.5, 0.45, 0.6, 14, 0.7)):
        mask = np.maximum(mask, crack(turned, y * s, px(246), px(766), k, 1.1 * s, px(3), tight=tight,
                                      leaks=(seed, share)).T)
    # a knot hole in the left leaf
    yy, xx = np.mgrid[0:luma.shape[0], 0:luma.shape[1]].astype(np.float32)
    mask = np.maximum(mask, 0.8 * gauss(np.hypot(xx - 469 * s, yy - 417 * s), 2.2 * s))
    return np.clip(mask, 0.0, 1.0)


def opening(albedo: np.ndarray) -> np.ndarray:
    """NW_CITY_FACHWERKBASEMENT_WINDOW_01: a small unglazed opening in the stone plinth with the far wall of the
    room seen in it. In texels of a 1024 map the opening is x 424..536, y 366..490."""
    s = albedo.shape[0] / 1024.0
    luma = albedo.mean(axis=2)
    yy, xx = np.mgrid[0:luma.shape[0], 0:luma.shape[1]].astype(np.float32)
    x0, x1, y0, y1 = 424 * s, 536 * s, 366 * s, 490 * s
    inside = np.minimum(np.minimum(xx - x0, x1 - xx), np.minimum(yy - y0, y1 - yy))
    soft = np.clip(inside / (9.0 * s), 0.0, 1.0)
    soft = soft * soft * (3.0 - 2.0 * soft)
    # the stones of the far wall keep their pattern: the lighter ones catch more of the candle
    box = (slice(int(y0), int(y1)), slice(int(x0), int(x1)))
    lo, hi = np.percentile(luma[box], [5, 95])
    stones = np.clip((luma - lo) / max(hi - lo, 1e-3), 0.0, 1.0)
    # softly: the block compression of the albedo must not draw outlines in the light
    stones = np.asarray(Image.fromarray((stones * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(2.5 * s)),
                        dtype=np.float32) / 255.0
    # only the dark of the opening glows, not the lit faces of the reveal
    dark = np.clip((0.16 - luma) / 0.10, 0.0, 1.0)
    return np.clip(soft * dark * (0.45 + 0.55 * stones), 0.0, 1.0)


# texture -> its mask, and the record's fields (windowglow.h)
WINDOWS = {
    'NW_CITY_WINDOW_03.TGA': {'mask': shutters, 'emissive': 1.0, 'window': {'lit': 0.6, 'until': 23.5}},
    'NW_CITY_FACHWERKBASEMENT_WINDOW_01.TGA': {'mask': opening, 'emissive': 0.45,
                                               'window': {'lit': 0.4, 'until': 23.0}},
}


def load_dds_codec():
    spec = importlib.util.spec_from_file_location('material_fixture', ROOT / 'scripts/content/test-material.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_orm(path: Path, orm: np.ndarray, dds) -> None:
    """DXT5 with box mips: a glowing gap fades with distance as its share of the texel does."""
    h, w = orm.shape[:2]
    levels = int(np.log2(min(w, h))) + 1
    header = [124, 0xA1007, h, w, (w // 4) * (h // 4) * 16, 0, levels] + [0] * 11
    header += [32, 4, int.from_bytes(b'DXT5', 'little'), 0, 0, 0, 0, 0, 0x401008, 0, 0, 0, 0]
    with path.open('wb') as out:
        out.write(b'DDS ' + struct.pack('<31I', *header))
        for level in range(levels):
            out.write(dds.bc3(dds.blocks(dds.quantize(orm))).tobytes())
            if level + 1 < levels:
                orm = dds.downsample(orm)


def clone(src: Path, dst: Path) -> None:
    if dst.exists():
        sys.exit(f'{dst} exists; give a new folder')
    dst.parent.mkdir(parents=True, exist_ok=True)
    if sys.platform == 'darwin' and subprocess.run(['cp', '-c', '-R', str(src), str(dst)], check=False).returncode == 0:
        return
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def cmd_pilot(args) -> int:
    src, out = args.content.expanduser().resolve(), args.output.expanduser().resolve()
    index = json.loads((src / 'materials/materials.json').read_text())
    missing = [name for name in WINDOWS if name not in index or 'orm' not in index[name]]
    if missing:
        sys.exit(f'no own record with an ORM for: {", ".join(missing)}')
    clone(src, out)
    dds = load_dds_codec()
    sheet = []
    for name, spec in WINDOWS.items():
        record = index[name]
        albedo = np.asarray(Image.open(src / 'materials' / record['albedo']).convert('RGB'), dtype=np.float32) / 255.0
        orm = np.asarray(Image.open(src / 'materials' / record['orm']).convert('RGB'), dtype=np.float32) / 255.0
        mask = spec['mask'](albedo)
        if mask.shape != orm.shape[:2]:
            mask = np.asarray(Image.fromarray((mask * 255).astype(np.uint8)).resize(orm.shape[1::-1], Image.LANCZOS),
                              dtype=np.float32) / 255.0
        target = out / 'materials' / record['orm']
        target.unlink()  # a clone shares its blocks with the source until it is written; a new file never does
        write_orm(target, np.concatenate([orm, mask[..., None]], axis=2), dds)
        record['emissive'] = spec['emissive']
        record['window'] = spec['window']
        lit = float((mask > 0.05).mean() * 100)
        print(f'{name}: glow on {lit:.2f} % of the texels, peak {mask.max():.2f}; emissive {spec["emissive"]}, '
              f'window {spec["window"]}')
        if args.sheet:
            glow = np.clip(albedo * 0.35 + mask[..., None] * np.array([1.0, 0.52, 0.16]), 0, 1)
            row = np.concatenate([albedo, np.repeat(mask[..., None], 3, axis=2), glow], axis=1)
            sheet.append((row * 255).astype(np.uint8))
    (out / 'materials/materials.json').unlink()
    (out / 'materials/materials.json').write_text(json.dumps(index, indent=1, ensure_ascii=False) + '\n')
    if args.sheet and sheet:
        Image.fromarray(np.concatenate(sheet, axis=0)).save(args.sheet)
        print(f'sheet (albedo | mask | albedo with the glow): {args.sheet}')
    print(f'content: {out}')
    return 0


def cmd_clip(args) -> int:
    """The shots of window-glow-dusk.bench as a clip, ten a second, the game's clock in the corner (ffmpeg)."""
    shots = sorted(args.shots.rglob('dusk-*.png'))
    if not shots:
        sys.exit(f'no dusk-*.png under {args.shots}')
    frames = args.output.with_suffix('.frames')
    frames.mkdir(parents=True, exist_ok=True)
    for i, shot in enumerate(shots):
        minute = 21 * 60 + int(shot.stem.split('-')[1]) * 3
        frame = Image.open(shot).convert('RGB')
        draw = ImageDraw.Draw(frame)
        label = f'{(minute // 60) % 24:02d}:{minute % 60:02d}'
        draw.rectangle((16, 14, 16 + 9 * len(label) * 2 + 12, 44), fill=(0, 0, 0))
        draw.text((22, 18), label, fill=(235, 225, 200), font_size=22)
        frame.save(frames / f'{i:03d}.png')
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', '10', '-i', str(frames / '%03d.png'),
                    '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '17', str(args.output)], check=True)
    shutil.rmtree(frames)
    print(f'clip: {args.output} ({len(shots)} frames)')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    pilot = sub.add_parser('pilot', help='a clone of a content folder with the windows glowing')
    pilot.add_argument('--content', type=Path, required=True)
    pilot.add_argument('--output', type=Path, required=True)
    pilot.add_argument('--sheet', type=Path)
    pilot.set_defaults(run=cmd_pilot)
    clip = sub.add_parser('clip', help='the shots of window-glow-dusk.bench as a clip')
    clip.add_argument('shots', type=Path)
    clip.add_argument('output', type=Path)
    clip.set_defaults(run=cmd_clip)
    args = parser.parse_args()
    return args.run(args)


if __name__ == '__main__':
    sys.exit(main())
