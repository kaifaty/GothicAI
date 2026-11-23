# One sheet per view of render_shapes.py stills: rows are shape sets, columns the mixes.
#   uv run --with pillow python sheet.py <stills dir> <set>... -- <label>...
import sys
from PIL import Image, ImageDraw, ImageFont
A = sys.argv[1:]
D, sets, labels = A[0], A[1:A.index('--')], A[A.index('--')+1:]
F = '/System/Library/Fonts/Supplemental/Arial.ttf'
W = 260
for view in ('front', 'quarter'):
    sheet = Image.new('RGB', (W*len(labels)+150, W*len(sets)+40), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    for j, lab in enumerate(labels):
        d.text((150+j*W+6, 8), lab, fill=(255, 214, 120), font=ImageFont.truetype(F, 15))
    for i, s in enumerate(sets):
        d.text((10, 40+i*W+W//2), s.upper(), fill=(255, 255, 255), font=ImageFont.truetype(F, 22))
        for j in range(len(labels)):
            sheet.paste(Image.open(f'{D}/{s}/{j:02d}_{view}.jpg').resize((W, W)), (150+j*W, 40+i*W))
    sheet.save(f'{D}/sheet_{view}.jpg', quality=88)
