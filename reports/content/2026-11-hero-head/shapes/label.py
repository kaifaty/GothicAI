# Header strip for compare.sh: the line's name and a label over each 480 px column.
#   python label.py <out.png> <line> <set weights>...
import sys
from PIL import Image, ImageDraw, ImageFont
out, line, cols = sys.argv[1], sys.argv[2], sys.argv[3:]
NAMES = {'mpfb v3fix': 'MPFB, с поправкой губ (сейчас)', 'mpfb v3': 'MPFB, без поправки',
         'ict v3fix': 'ICT, с поправкой губ', 'mark v3': 'Mark, без поправки'}
F = '/System/Library/Fonts/Supplemental/Arial.ttf'
im = Image.new('RGB', (480*len(cols), 64), (22, 22, 22))
d = ImageDraw.Draw(im)
for i, c in enumerate(cols):
    d.text((i*480+240, 22), NAMES.get(c, c), fill=(255, 214, 120), font=ImageFont.truetype(F, 22), anchor='mm')
d.text((8, 50), line, fill=(160, 160, 160), font=ImageFont.truetype(F, 14), anchor='lm')
im.save(out)
