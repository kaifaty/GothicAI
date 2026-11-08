"""Pairs from two hero-body.bench runs: top row the original, bottom row ours.
python compare.py <orig dir> <ours dir> <out.jpg> shots[:a,b,...]|<clip>:<t1,t2,...> [x0,y0,x1,y1] [scale]"""
import sys, subprocess, tempfile, os
from PIL import Image, ImageDraw, ImageFont
a, b, out, what = sys.argv[1:5]
box = tuple(int(v) for v in sys.argv[5].split(',')) if len(sys.argv) > 5 else (660, 140, 1260, 700)
scale = float(sys.argv[6]) if len(sys.argv) > 6 else 0.75
def frames(d):
    if what.startswith('shots'):
        names = what.split(':')[1].split(',') if ':' in what else ('front', 'quarter', 'side', 'back', 'upper')
        return [(n, Image.open(f'{d}/{n}.png').convert('RGB')) for n in names]
    clip, ts = what.split(':')
    res = []
    for t in ts.split(','):
        f = tempfile.mktemp(suffix='.png')
        subprocess.run(['ffmpeg', '-v', 'error', '-ss', t, '-i', f'{d}/{clip}.mp4', '-frames:v', '1', f], check=True)
        res.append((f'{clip} {t}s', Image.open(f).convert('RGB'))); os.unlink(f)
    return res
rows = [frames(a), frames(b)]
tile = [[im.crop(box) for _, im in r] for r in rows]
tw, th = int((box[2] - box[0]) * scale), int((box[3] - box[1]) * scale)
sheet = Image.new('RGB', (tw * len(tile[0]), th * 2 + 28), (20, 20, 20))
dr = ImageDraw.Draw(sheet); font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 20)
for r, row in enumerate(tile):
    for c, im in enumerate(row):
        sheet.paste(im.resize((tw, th)), (c * tw, 28 + r * th))
labels = os.environ.get('LABELS', '').split(',') if os.environ.get('LABELS') else None
for c, (n, _) in enumerate(rows[0]):
    dr.text((c * tw + 6, 3), labels[c] if labels else n, fill=(230, 230, 230), font=font)
sheet.save(out, quality=88)
