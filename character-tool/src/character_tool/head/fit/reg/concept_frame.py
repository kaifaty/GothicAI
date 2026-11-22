# The concept views laid into the 1200 px landmark frames of reg_export.py (views.json), so landmarks are picked on
# the concept and get their depth from the reference. Also blends each with the reference render of the same frame.
#   python concept_frame.py <reg dir> params.json front_ref.png profile_ref.png
import sys, os, json
from PIL import Image
D, P = sys.argv[1], json.load(open(sys.argv[2])); V = json.load(open(os.path.join(D, 'views.json')))
N, s, eye = V['N'], V['s'], V['eye']; mpp = 0.0615 / P['ipd_px']
for name, src, (ex, ey), wc in (('front', sys.argv[3], P['front_eye'], 0.0), ('profile', sys.argv[4], P['profile_eye'], eye[1])):
    cen = V['views'][name]['loc'][0 if name == 'front' else 1]            # world coordinate at the frame's centre column
    k = s / mpp                                                             # concept px per frame px
    # frame (u,v) -> world (cen+(u-N/2)s, eye_z-0.03-(v-N/2)s) -> concept (ex+(w-wc)/mpp, ey-(z-eye_z)/mpp)
    a = (k, 0, ex + (cen - wc - N / 2 * s) / mpp, 0, k, ey + (0.03 - N / 2 * s) / mpp)
    im = Image.open(src).convert('RGB').transform((N, N), Image.AFFINE, a, Image.BICUBIC, fillcolor=(122, 124, 128))
    im.save(os.path.join(D, f'concept_{name}.png'))
    ref = os.path.join(D, f'lm_ref_{name}.png')
    if os.path.exists(ref):
        Image.blend(im, Image.open(ref).convert('RGB'), 0.5).save(os.path.join(D, f'blend_{name}.png'))
