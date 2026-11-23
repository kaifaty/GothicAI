# Stills and strips for cues.sh (pillow). Frames are f%04d.jpg at 30 fps, frame k shows time (k-1)/30.
#   python sheet.py a <out dir>             contact sheet of clip A: plain | gesture at the apex of each gesture
#   python sheet.py b <out dir>             contact sheet of clip B: before | apex of the gesture and of the leak
#   python sheet.py strip <out.png> <width px> <text> [<small text>]   header strip over a cell or a segment
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT = '/System/Library/Fonts/Supplemental/Arial.ttf'
YELLOW, GREY, BG = (255, 214, 120), (170, 170, 170), (22, 22, 22)

def frame(d, t):
    fs = sorted(Path(d).glob('f*.jpg'))
    k = min(max(int(round(t*30))+1, 1), len(fs))
    return Image.open(Path(d)/f'f{k:04d}.jpg').convert('RGB')

def tile(im, size, text):
    im = im.resize((size, size), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, size, 26), fill=BG)
    d.text((6, 13), text, fill=YELLOW, font=ImageFont.truetype(FONT, 17), anchor='lm')
    return im

def grid(tiles, cols, size):
    rows = (len(tiles)+cols-1)//cols
    out = Image.new('RGB', (cols*size+(cols-1)*4, rows*size+(rows-1)*4), (60, 60, 60))
    for i, t in enumerate(tiles):
        if t is not None:
            out.paste(t, ((i % cols)*(size+4), (i//cols)*(size+4)))
    return out

def sheet_a(out):
    plan = json.loads((out/'plan.json').read_text())
    F, S, tiles = out/'frames'/'a', 300, []
    for c in plan['a']['cells'][1:]:
        t = c['apex'][0]
        tiles += [tile(frame(F/'plain', t), S, f"без жестов {t:.2f} с"), tile(frame(F/c['key'], t), S, c['ru'])]
    grid(tiles, 8, S).save(out/'sheet-a.jpg', quality=90)

def sheet_b(out):
    plan = json.loads((out/'plan.json').read_text())
    F, S, tiles = out/'frames'/'b', 280, []
    for s in plan['b']['segments']:
        d = F/f"seg{s['n']:02d}"
        g0, ga = s['gesture_start'], s['gesture_apex']
        tiles += [tile(frame(d, max(g0-.05, 0)), S, f"{s['n']}: до жеста"), tile(frame(d, ga), S, f"{s['n']}: {s['gesture_ru']}")]
        lk = s.get('leak')
        tiles += ([tile(frame(d, max(lk['start']-.05, 0)), S, f"{s['n']}: до утечки"),
                   tile(frame(d, lk['apex']), S, f"{s['n']}: {lk['felt']} {lk['face']}")] if lk else [None, None])
    grid(tiles, 8, S).save(out/'sheet-b.jpg', quality=90)

def strip(path, width, text, small=''):
    im = Image.new('RGB', (width, 64), BG)
    d = ImageDraw.Draw(im)
    d.text((width//2, 30), text, fill=YELLOW, font=ImageFont.truetype(FONT, 30), anchor='mm')
    if small:
        d.text((8, 54), small, fill=GREY, font=ImageFont.truetype(FONT, 14), anchor='lm')
    im.save(path)

if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'a':
        sheet_a(Path(sys.argv[2]))
    elif mode == 'b':
        sheet_b(Path(sys.argv[2]))
    else:
        strip(sys.argv[2], int(sys.argv[3]), sys.argv[4], *sys.argv[5:6])
