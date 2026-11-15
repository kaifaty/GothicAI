#!/usr/bin/env python3
"""The acceptance clips of the lamps' screen ray (../README.md, "Шаг 4") side by side: the two runs of
lamp-contact-clips.bench, without the ray on the left and with it on the right, the middle half of each frame
pixel for pixel and a label over each (ffmpeg; the labels are drawn by Pillow, not every ffmpeg has drawtext).

    uv run --no-project --with pillow python lamp_contact_clips.py <run without the ray> <run with it> <out dir>
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONTS = ('/System/Library/Fonts/Supplemental/Arial.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')


def label(text: str, font: ImageFont.FreeTypeFont, path: Path) -> Path:
    width = int(font.getlength(text)) + 24
    img = Image.new('RGBA', (width, 44), (0, 0, 0, 150))
    ImageDraw.Draw(img).text((12, 6), text, fill=(255, 255, 255, 255), font=font)
    img.save(path)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('without', type=Path)
    parser.add_argument('with_ray', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--font', type=Path, help='a font with Cyrillic for the labels')
    args = parser.parse_args()
    font_path = args.font or next((Path(f) for f in FONTS if Path(f).exists()), None)
    if font_path is None:
        sys.exit('no font for the labels: pass --font')
    font = ImageFont.truetype(str(font_path), 26)
    args.out.mkdir(parents=True, exist_ok=True)
    clips = sorted(p.name for p in args.without.glob('*.mp4') if (args.with_ray / p.name).exists())
    if not clips:
        sys.exit(f'no clips common to {args.without} and {args.with_ray}')
    with tempfile.TemporaryDirectory() as tmp:
        off = label('без луча', font, Path(tmp) / 'off.png')
        on = label('с лучом', font, Path(tmp) / 'on.png')
        for clip in clips:
            out = args.out / f'lamp-contact-{clip}'
            graph = ('[0:v]crop=iw/2:ih[a0];[a0][2:v]overlay=16:16[a];'
                     '[1:v]crop=iw/2:ih[b0];[b0][3:v]overlay=16:16[b];[a][b]hstack=inputs=2:shortest=1[v]')
            subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', str(args.without / clip),
                            '-i', str(args.with_ray / clip), '-i', str(off), '-i', str(on),
                            '-filter_complex', graph, '-map', '[v]',
                            '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '16', str(out)], check=True)
            print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
