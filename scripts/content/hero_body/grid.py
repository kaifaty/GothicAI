import sys, json
from PIL import Image, ImageDraw, ImageFont
d = sys.argv[1]
meta = json.load(open(d + '/views.json'))
W, H = meta['res']; ppm = H / meta['ortho_scale']; cy = meta['cy']
def to_px(u, v): return W / 2 + u * ppm, H / 2 - (v - cy) * ppm
font = ImageFont.load_default(size=18)
for name in ('front', 'left', 'back'):
    im = Image.open(f'{d}/{name}.png').convert('RGBA')
    bg = Image.new('RGBA', im.size, (235, 235, 235, 255)); bg.alpha_composite(im); im = bg
    dr = ImageDraw.Draw(im)
    for cm in range(-40, 41):
        x, _ = to_px(cm / 100, 0)
        dr.line([(x, 0), (x, H)], fill=(255, 0, 0, 255) if cm % 5 == 0 else (120, 120, 255, 110), width=1)
        if cm % 5 == 0: dr.text((x + 2, 4), str(cm), fill=(200, 0, 0), font=font)
    for cm in range(0, 100):
        _, y = to_px(0, cm / 100)
        dr.line([(0, y), (W, y)], fill=(255, 0, 0, 255) if cm % 5 == 0 else (120, 120, 255, 110), width=1)
        if cm % 5 == 0: dr.text((4, y - 20), str(cm), fill=(200, 0, 0), font=font)
    im.convert('RGB').save(f'{d}/{name}-grid.png')
