# skin_ladder.py <sheet.jpg> --matte <name> <name>=<run dir> ... : the ladder of the bare skin's roughness in the
# game's frames (../skin-sun.md, "Лесенка шероховатости при настоящей экспозиции"). Each run dir holds the frames of
# scripts/bench/npc-head-look.bench, 1920x1080, of the head with one roughness; <matte> names the run where the game
# puts no highlight on the skin (roughness from 0.8). Two runs of one content give the same frames pixel for pixel,
# so a step is compared with the matte run directly.
# Prints:
# - per light, what the highlight adds to a zone of the face, per cent by channel: the zone's mean (linear light)
#   over the neck's of the same frame, against the same ratio of the matte run. The neck is the body: it has no
#   material and does not change with the head's maps, so the frame's exposure drops out. And green and blue over
#   red on the cheeks and on the neck (medians);
# - per shot, the share of the face's pixels where the frame is brighter than the matte one by more than a half and
#   by more than twice, and how many times at the brightest (the 99.99th percentile): the white streaks.
# The sheet: the places where the steps differ, by the steps.
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

a = sys.argv[1:]; i = a.index('--matte'); MATTE = a[i+1]; del a[i:i+2]
OUT = a[0]; RUNS = [s.split('=', 1) for s in a[1:]]; matte = dict(RUNS)[MATTE]
lin = lambda c: np.where(c > 0.04045, ((c+0.055)/1.055)**2.4, c/12.92)
LUM = np.array([0.2126, 0.7152, 0.0722])
FACE = (700, 60, 1220, 720)
# shot, title, zones: boxes x0, y0, x1, y1 of the frame
LIGHTS = (
    ('sunface_front', 'солнце в лицо', {
        'forehead': [(880, 130, 1040, 190)], 'nose': [(930, 370, 990, 430)],
        'cheeks': [(790, 400, 850, 460), (1080, 400, 1140, 460)], 'neck': [(830, 730, 880, 800), (1040, 730, 1090, 800)]}),
    ('sun_front', 'солнце сверху-сзади', {
        'forehead': [(880, 130, 1040, 190)], 'nose': [(930, 370, 990, 430)],
        'cheeks': [(790, 400, 850, 460), (1080, 400, 1140, 460)], 'neck': [(920, 720, 1000, 780)]}),
    ('tower_front', 'факел', {
        'forehead': [(880, 150, 1040, 220)], 'nose': [(930, 380, 990, 450)],
        'cheeks': [(790, 420, 850, 480), (1090, 420, 1150, 480)], 'neck': [(900, 740, 1020, 800)]}))
SHOTS = ('tower_front', 'tower_quarter', 'tower_profile', 'sun_front', 'sun_quarter', 'sun_quarter_other', 'sun_profile',
         'sunface_front', 'sunface_quarter', 'sunface_quarter_other', 'sunface_bust')
# the sheet's rows: shot, title, crop
ROWS = (('sunface_front', 'солнце в лицо', FACE),
        ('sun_quarter', 'солнце сверху-сзади: висок', (700, 60, 1220, 560)),
        ('sun_front', 'солнце сверху-сзади: лоб', (820, 100, 1100, 240)),
        ('tower_front', 'факел: щека у уха', (720, 330, 1000, 640)))


def frame(run, shot):
    return lin(np.asarray(Image.open('%s/%s.png' % (run, shot)).convert('RGB')).astype(np.float64)/255)


def zone(im, boxes):
    return np.concatenate([im[y0:y1, x0:x1].reshape(-1, 3) for x0, y0, x1, y1 in boxes])


for shot, title, zones in LIGHTS:
    print('== %s (%s): what the highlight adds, %% r g b' % (title, shot))
    print('%-6s | %-17s | %-17s | %-17s | %-12s | %-12s' % ('step', 'forehead', 'nose', 'cheeks', 'cheeks g b/r', 'neck g b/r'))
    ref = frame(matte, shot); ref = {z: zone(ref, b).mean(0) for z, b in zones.items()}
    for name, run in RUNS:
        im = frame(run, shot); m = {z: zone(im, b).mean(0) for z, b in zones.items()}
        add = lambda z: '%5.1f %5.1f %5.1f' % tuple(100*((m[z]/m['neck'])/(ref[z]/ref['neck'])-1))
        c, n = np.median(zone(im, zones['cheeks']), 0), np.median(zone(im, zones['neck']), 0)
        print('%-6s | %s | %s | %s | %.2f %.2f    | %.2f %.2f' % (name, add('forehead'), add('nose'), add('cheeks'), c[1]/c[0], c[2]/c[0], n[1]/n[0], n[2]/n[0]))
print('== the face over the matte run: %% of pixels over 1.5 times, over 2 times; times at the brightest')
print('%-22s | ' % 'shot' + ' | '.join('%-17s' % name for name, _ in RUNS))
x0, y0, x1, y1 = FACE
for shot in SHOTS:
    ref = frame(matte, shot)[y0:y1, x0:x1] @ LUM; out = []
    for name, run in RUNS:
        r = (frame(run, shot)[y0:y1, x0:x1] @ LUM)/np.maximum(ref, 1e-4); r = np.where(ref > 0.004, r, 1)   # not in the black of the hair
        out.append('%5.2f %5.2f %5.1f' % (100*(r > 1.5).mean(), 100*(r > 2).mean(), np.percentile(r, 99.99)))
    print('%-22s | ' % shot + ' | '.join(out))

try:
    FONT = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 20)
except OSError:
    FONT = ImageFont.load_default()
CW, GAP, BG = 380, 6, (20, 20, 20)


def cell(run, shot, box, label):
    im = Image.open('%s/%s.png' % (run, shot)).convert('RGB').crop(box)
    im = im.resize((CW, round(im.height*CW/im.width)), Image.LANCZOS)
    if label:
        d = ImageDraw.Draw(im); d.rectangle([0, 0, d.textlength(label, font=FONT)+12, 28], fill=(0, 0, 0))
        d.text((6, 3), label, font=FONT, fill=(255, 255, 255))
    return im


rows = [[cell(run, shot, box, (name+': ' if k == 0 else '')+title if j == 0 else name if k == 0 else '')
         for j, (name, run) in enumerate(RUNS)] for k, (shot, title, box) in enumerate(ROWS)]
sheet = Image.new('RGB', (CW*len(RUNS)+GAP*(len(RUNS)-1), sum(r[0].height for r in rows)+GAP*(len(rows)-1)), BG); y = 0
for r in rows:
    for j, c in enumerate(r):
        sheet.paste(c, (j*(CW+GAP), y))
    y += r[0].height+GAP
sheet.save(OUT, quality=90); print(OUT, sheet.size)
