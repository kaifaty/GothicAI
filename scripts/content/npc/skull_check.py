"""Does the bald view leave room for the hair? Proportions of the skull against the concept with hair.

  python skull_check.py concept_sheet.png bald_sheet.png [bald2.png ...]

Each sheet is the front view and the left profile side by side on the grey concept background. A generator asked
to remove the hair tends to fill the outline of the hair with skull. The check measures, in the front view:
- how far the top of the bare skull is below the top of the hair, in mm (61.5 mm between the irises);
- the same on the sides above the ears;
- the height of the skull above the eyes against the height of the face below them (about 1.0 in people);
and in the profile how far the back of the skull is in front of the back of the hair.
Needs the venv with mediapipe: ~/.local/share/gothic-ai/mpfb/venv-mp.
"""
import os, sys
import numpy as np
from PIL import Image
import mediapipe as mp
from mediapipe.tasks.python import BaseOptions, vision

MODEL = os.environ.get('FACE_LANDMARKER', os.path.expanduser('~/.local/share/gothic-ai/models/mediapipe/face_landmarker.task'))
det = vision.FaceLandmarker.create_from_options(vision.FaceLandmarkerOptions(base_options=BaseOptions(model_asset_path=MODEL), num_faces=1))


def read(path):
    a = np.asarray(Image.open(path).convert('RGB')); h, w = a.shape[:2]; half = w // 2
    bg = np.median(np.concatenate([a[0], a[-1], a[:, 0]]), axis=0)
    mask = np.abs(a.astype(int) - bg).max(2) > 14
    res = det.detect(mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(a[:, :half])))
    p = np.array([[q.x * half, q.y * h] for q in res.face_landmarks[0]])
    eye = (p[468, 1] + p[473, 1]) / 2; mm = 61.5 / np.linalg.norm(p[468] - p[473])
    f, s = mask[:, :half], mask[:, half:]
    top = np.nonzero(f.any(1))[0][0]
    side_rows = range(int(top + 0.35 * (eye - top)), int(top + 0.75 * (eye - top)))          # above the ears, below the crown
    width = np.median([np.nonzero(f[y])[0][-1] - np.nonzero(f[y])[0][0] for y in side_rows])
    back_rows = range(int(top + 0.4 * (eye - top)), int(eye))
    back = np.median([np.nonzero(s[y])[0][-1] for y in back_rows])                           # the face looks left: the back is on the right
    return dict(mm=mm, eye=eye, top=top, chin=p[152, 1], width=width, back=back)


c = read(sys.argv[1])
print('%-28s skull above the eyes / face below: %.2f (with hair)' % (os.path.basename(sys.argv[1]), (c['eye'] - c['top']) / (c['chin'] - c['eye'])))
for path in sys.argv[2:]:
    b = read(path); mm = (b['mm'] + c['mm']) / 2
    print('%-28s skull above the eyes / face below: %.2f | room for hair, mm: top %.1f, each side %.1f, back %.1f' % (
        os.path.basename(path), (b['eye'] - b['top']) / (b['chin'] - b['eye']),
        ((b['top'] - b['eye']) - (c['top'] - c['eye'])) * mm, (c['width'] - b['width']) / 2 * mm, (c['back'] - b['back']) * mm))
