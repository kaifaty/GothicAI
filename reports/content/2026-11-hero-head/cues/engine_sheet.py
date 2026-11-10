# The sheet of scripts/bench/face-cues.bench (the gestures in the engine) or face-leaks.bench (the leaks): a row per
# gesture or leak, the frame without it and the frame at its apex, cropped to the face (pillow).
#   uv run --with pillow python engine_sheet.py <bench out dir> <sheet.jpg> [<crop x0 y0 x1 y1 as parts of the frame>]
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FELT = dict(friendly='радость', angry='злость', hostile='неприязнь', afraid='страх', pain='боль')
HALF = dict(upper='верх', lower='низ')
LEAKS = [f'leak-{f}-{h}' for f in FELT for h in HALF]
ORDER = 'flash doubt smirk press disgust squint frown chin wince npc-smirk'.split() + LEAKS
RU = dict(flash='вспышка бровей', doubt='сомнение L', smirk='ухмылка L', press='поджатые губы', disgust='брезгливость',
          squint='прищур', frown='хмурится', chin='подбородок', wince='поморщился', **{'npc-smirk': 'NPC: ухмылка R'},
          **{f'leak-{f}-{h}': f'прорыв: {FELT[f]}, {HALF[h]}' for f in FELT for h in HALF})
FONTS = ['/System/Library/Fonts/Supplemental/Arial.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf']
YELLOW, BG = (255, 214, 120), (22, 22, 22)

def font(size):
    for f in FONTS:
        if Path(f).exists():
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()

def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    crop = [float(x) for x in sys.argv[3:7]] if len(sys.argv) >= 7 else [0.25, 0.0, 0.75, 1.0]
    rows = [g for g in ORDER if (src/f'{g}-rest.png').exists() and (src/f'{g}-apex.png').exists()]
    tiles = []
    for g in rows:
        pair = []
        for k in ('rest', 'apex'):
            im = Image.open(src/f'{g}-{k}.png').convert('RGB')
            w, h = im.size
            im = im.crop((int(crop[0]*w), int(crop[1]*h), int(crop[2]*w), int(crop[3]*h)))
            pair.append(im)
        tiles.append((g, pair))
    tw, th = tiles[0][1][0].size
    scale = min(1.0, 420/th)
    tw, th = int(tw*scale), int(th*scale)
    cols = 2   # pairs side by side: two gestures per row
    rowsn = (len(tiles)+cols-1)//cols
    head = 28
    sheet = Image.new('RGB', (cols*(2*tw+12)+(cols-1)*16, rowsn*(th+head+8)), (60, 60, 60))
    d = ImageDraw.Draw(sheet)
    for i, (g, pair) in enumerate(tiles):
        x0 = (i % cols)*(2*tw+12+16)
        y0 = (i//cols)*(th+head+8)
        d.rectangle((x0, y0, x0+2*tw+12, y0+head), fill=BG)
        rest = 'без прорыва' if g.startswith('leak-') else 'без жеста'
        d.text((x0+8, y0+head//2), f'{RU.get(g, g)}: {rest} | пик', fill=YELLOW, font=font(18), anchor='lm')
        for j, im in enumerate(pair):
            sheet.paste(im.resize((tw, th), Image.LANCZOS), (x0+j*(tw+12), y0+head))
    sheet.save(out, quality=90)
    print(out, sheet.size, len(tiles), 'pairs')

if __name__ == '__main__':
    main()
