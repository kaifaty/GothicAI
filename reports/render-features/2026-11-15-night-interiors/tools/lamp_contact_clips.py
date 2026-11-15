#!/usr/bin/env python3
"""The acceptance clips of the lamps' screen ray (../README.md, "Шаг 4") side by side: the two runs of
lamp-contact-clips.bench, without the ray on the left and with it on the right, a label over each half (ffmpeg).

    python3 lamp_contact_clips.py <run without the ray> <run with it> <out dir> [--font <ttf>]
"""
import argparse
import subprocess
import sys
from pathlib import Path

FONTS = ('/System/Library/Fonts/Supplemental/Arial.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('without', type=Path)
    parser.add_argument('with_ray', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--font', type=Path, help='a font with Cyrillic for the labels')
    args = parser.parse_args()
    font = args.font or next((Path(f) for f in FONTS if Path(f).exists()), None)
    if font is None:
        sys.exit('no font for the labels: pass --font')
    args.out.mkdir(parents=True, exist_ok=True)
    clips = sorted(p.name for p in args.without.glob('*.mp4') if (args.with_ray / p.name).exists())
    if not clips:
        sys.exit(f'no clips common to {args.without} and {args.with_ray}')

    def label(text: str) -> str:
        return (f"drawtext=fontfile='{font}':text='{text}':fontsize=30:fontcolor=white:box=1:boxcolor=black@0.6:"
                'boxborderw=8:x=20:y=20')

    for clip in clips:
        out = args.out / f'lamp-contact-{clip}'
        graph = (f'[0:v]{label("без луча")}[a];[1:v]{label("с лучом")}[b];'
                 '[a][b]hstack=inputs=2:shortest=1,scale=iw/2:-2[v]')
        subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', str(args.without / clip),
                        '-i', str(args.with_ray / clip), '-filter_complex', graph, '-map', '[v]',
                        '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '16', str(out)], check=True)
        print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
