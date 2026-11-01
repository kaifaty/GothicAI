"""OWN_HUT_SMOKE: the particle texture of the r5 smoke, made from the base, not kept in git (a derivative of an original).

usage: own_texture.py BASE_TEXTURES_DIR OUT.png
Alpha of FIRESMOKE (static, uneven clumps, no plateau, peak 0.867) times a round smooth window: 1 inside r 0.45,
0 beyond r 0.78 of the half-side, so the alpha is zero in the outer 12 % of the square and no sprite edge shows.
RGB is white: the colour of the smoke is the vertex colour of the PFX alone, as the research card counts it
(FIRESMOKE's own texel, about 122/111/100, would make a grey of 75-115 three times darker). The game takes it
through OPENGOTHIC_TEXTURES (upper-case stem; visName_S OWN_HUT_SMOKE.TGA) until particles read content/materials."""
import sys
import numpy as np
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
alpha = np.asarray(Image.open(f'{src}/FIRESMOKE.dds').convert('RGBA'), float)[..., 3] / 255
h, w = alpha.shape
yy, xx = np.mgrid[0:h, 0:w]
r = np.hypot((xx + .5) / w * 2 - 1, (yy + .5) / h * 2 - 1)
t = np.clip((0.78 - r) / (0.78 - 0.45), 0, 1)
alpha *= t * t * (3 - 2 * t)
Image.fromarray(np.dstack([np.full((h, w, 3), 255.), alpha * 255]).round().astype(np.uint8), 'RGBA').save(out)
print(out, 'peak %.3f mean %.3f' % (alpha.max(), alpha.mean()))
