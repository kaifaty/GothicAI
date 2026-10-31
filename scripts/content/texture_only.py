#!/usr/bin/env python3
"""Texture Only: a batch remaster of textures whose original form is right and only its craft is not,
by edits of the original (docs/remaster-pipeline.md, «Правка оригинальной текстуры»).

    texture_only.py prepare NW_….TGA … --originals DIR --out DIR
    texture_only.py assemble NW_….TGA EDIT.png --layout DIR/<name>.json --out ASSEMBLED.png

prepare writes, for each texture, the picture the generator edits, its brief and its layout. A generator
canvas is 1024 x 1024, 1536 x 1024 or 1024 x 1536; a long strip (a kerb 8:1, a beam 4:1) is cut into
pieces stacked top to bottom so it fills the canvas, the rest grey. The brief is filled from the city
survey (data/content/city-materials.csv): what the texture depicts, its colours and wear. assemble cuts
the edit back into the strip at the edit's own resolution; edit_texture.py then takes its colour back
from the original and makes its maps.

The original's pixels go only to the generator and to edit_texture.py (the user's decision of
2026-10-23); the output folder must be outside the repository.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SURVEY = ROOT / 'data/content/city-materials.csv'
CANVASES = ((1024, 1024), (1536, 1024), (1024, 1536))
GREY = (128, 128, 128)

BRIEF = """The file {input} is the input image. Use your image_gen tool to EDIT that image (pass it as the input/reference image), with exactly the prompt below, output {width}x{height}. Reply only with the generated file's absolute path and the exact prompt you passed. Do not judge or describe the image.

PROMPT:
Edit the input image: it is a game texture from a 2002 game — {depicts}; {colour}; {weathering}.{pieces} Remaster it as the same texture, better made. Keep exactly: the layout (every stone's, plank's or strap's position, size and shape, every joint), the colours and their brightness, the moss, dirt and damp, the mood. Improve only the craft: crisp photographic detail at the scale of one pixel — real stone faces with sharp broken edges and granular grain, deep joints, fibrous wood grain, rusty iron where there is iron; every stone face a flat or gently curved split plane with sharp broken edges and even granular grain. No drawn pattern on any surface: no wavy, wormy or scribbled strokes, no hatching, no engraving lines, no honeycomb or scale texture; no blur, no painterly smoothing, no JPEG artefacts. Flat even light as in the input, no new cast shadows. {seamless}
"""
STACKED = (' The image holds {count} pieces of one long strip texture stacked top to bottom: the right end of each '
           'piece continues into the left end of the piece below it. Keep the pieces where they are and keep '
           'the plain grey margins grey.')
SEAMLESS = 'The texture must stay seamless when tiled left-right and top-bottom, as the input is.'


def survey(texture: str) -> dict:
    for row in csv.DictReader(SURVEY.open()):
        if row['texture'] == texture:
            return row
    raise SystemExit(f'{texture}: not in {SURVEY}')


def layout_for(size) -> dict:
    """Pieces and canvas for a texture of this size: the strip cut so the stack is near the canvas's shape."""
    width, height = size
    best = None
    for canvas in CANVASES:
        for count in range(1, 9):
            if width % count:
                continue
            piece = (width // count, height)
            stack = (piece[0], piece[1] * count)
            scale = min(canvas[0] / stack[0], canvas[1] / stack[1])
            filled = stack[0] * stack[1] * scale * scale / (canvas[0] * canvas[1])
            if best is None or filled > best['filled'] + 1e-9:
                best = dict(canvas=canvas, count=count, piece=piece, scale=scale, filled=filled)
    canvas, count, scale = best['canvas'], best['count'], best['scale']
    stack = (round(best['piece'][0] * scale), round(best['piece'][1] * scale) * count)
    origin = ((canvas[0] - stack[0]) // 2, (canvas[1] - stack[1]) // 2)
    return dict(size=list(size), canvas=list(canvas), count=count, piece=list(best['piece']),
                scaled_piece=[stack[0], stack[1] // count], origin=list(origin))


def stack(image: Image.Image, layout: dict) -> Image.Image:
    canvas = Image.new('RGB', tuple(layout['canvas']), GREY)
    pw, ph = layout['piece']
    sw, sh = layout['scaled_piece']
    for i in range(layout['count']):
        piece = image.crop((i * pw, 0, (i + 1) * pw, ph)).resize((sw, sh), Image.LANCZOS)
        canvas.paste(piece, (layout['origin'][0], layout['origin'][1] + i * sh))
    return canvas


def unstack(edit: Image.Image, layout: dict) -> Image.Image:
    """The edit back in the original's shape, at the edit's resolution."""
    edit = edit.convert('RGB')
    k = edit.width / layout['canvas'][0]                 # the generator may return a larger canvas (1254 for 1024)
    sw, sh = (round(v * k) for v in layout['scaled_piece'])
    x0, y0 = (round(v * k) for v in layout['origin'])
    strip = Image.new('RGB', (sw * layout['count'], sh))
    for i in range(layout['count']):
        strip.paste(edit.crop((x0, y0 + i * sh, x0 + sw, y0 + (i + 1) * sh)), (i * sw, 0))
    return strip


def stem(texture: str) -> str:
    return texture.removesuffix('.TGA')


def cmd_prepare(args) -> int:
    args.out.mkdir(parents=True, exist_ok=True)
    for texture in args.textures:
        row = survey(texture)
        image = Image.open(args.originals / f'{stem(texture)}-native.png').convert('RGB')
        layout = layout_for(image.size)
        source = args.out / f'{stem(texture)}.png'
        stack(image, layout).save(source)
        pieces = STACKED.format(count=layout['count']) if layout['count'] > 1 else ''
        brief = BRIEF.format(input=source, width=layout['canvas'][0], height=layout['canvas'][1],
                             depicts=row['depicts'], colour=row['colour'], weathering=row['weathering'],
                             pieces=pieces, seamless=SEAMLESS)
        (args.out / f'{stem(texture)}-brief.md').write_text(brief)
        (args.out / f'{stem(texture)}.json').write_text(json.dumps(
            dict(texture=texture, period=[float(row['uv_m_u']), float(row['uv_m_v'])], **layout), indent=1))
        print(source, layout['count'], 'x'.join(map(str, layout['canvas'])))
    return 0


def cmd_assemble(args) -> int:
    layout = json.loads(args.layout.read_text())
    unstack(Image.open(args.edit), layout).save(args.out)
    print(args.out)
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('prepare', help='generator inputs, briefs and layouts')
    p.add_argument('textures', nargs='+')
    p.add_argument('--originals', type=Path, required=True, help='folder of <NAME>-native.png from original_view.py')
    p.add_argument('--out', type=Path, required=True)
    p.set_defaults(run=cmd_prepare)
    p = sub.add_parser('assemble', help='an edit back into the original texture shape')
    p.add_argument('texture')
    p.add_argument('edit', type=Path)
    p.add_argument('--layout', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.set_defaults(run=cmd_assemble)
    args = parser.parse_args(argv)
    return args.run(args)


if __name__ == '__main__':
    raise SystemExit(main())
