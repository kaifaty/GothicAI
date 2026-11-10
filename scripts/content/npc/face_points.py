"""Face landmarks of a picture: the concept, a render of the dense model, a render of our head.

  python face_points.py out_dir name=image.png[:x0,y0,x1,y1] ...

Runs MediaPipe Face Landmarker (478 points, the model file is outside git) on each image or crop and
writes <out_dir>/<name>.json: points in pixels of the whole image, z in the same units (relative depth),
and <out_dir>/<name>.jpg with the points drawn. Prints which images gave a face.
Needs the venv with mediapipe: ~/.local/share/gothic-ai/mpfb/venv-mp.
"""
import json, os, sys
import numpy as np
from PIL import Image, ImageDraw
import mediapipe as mp
from mediapipe.tasks.python import BaseOptions, vision

MODEL = os.environ.get('FACE_LANDMARKER', os.path.expanduser('~/.local/share/gothic-ai/models/mediapipe/face_landmarker.task'))
# index groups of the 478-point mesh that the wrap uses; the full set is saved as well
GROUPS = {
    'lip_upper_outer': [61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291],
    'lip_lower_outer': [61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291],
    'lip_upper_inner': [78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308],
    'lip_lower_inner': [78, 95, 88, 178, 87, 14, 317, 402, 318, 324, 308],
    'eye_r_upper': [33, 246, 161, 160, 159, 158, 157, 173, 133],     # the subject's right eye, on the left of a front picture
    'eye_r_lower': [33, 7, 163, 144, 145, 153, 154, 155, 133],
    'eye_l_upper': [263, 466, 388, 387, 386, 385, 384, 398, 362],
    'eye_l_lower': [263, 249, 390, 373, 374, 380, 381, 382, 362],
    'brow_r': [70, 63, 105, 66, 107], 'brow_l': [300, 293, 334, 296, 336],
    'nose': [1, 2, 4, 5, 6, 168, 98, 327, 129, 358],
    'jaw': [234, 93, 132, 58, 172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361, 323, 454],
    'iris_r': [468, 469, 470, 471, 472], 'iris_l': [473, 474, 475, 476, 477],
}


def main():
    out = sys.argv[1]; os.makedirs(out, exist_ok=True)
    det = vision.FaceLandmarker.create_from_options(vision.FaceLandmarkerOptions(
        base_options=BaseOptions(model_asset_path=MODEL), num_faces=1, min_face_detection_confidence=0.3))
    for arg in sys.argv[2:]:
        name, src = arg.split('=', 1); box = None
        if ':' in src and src.rsplit(':', 1)[1].count(',') == 3:
            src, b = src.rsplit(':', 1); box = [int(v) for v in b.split(',')]
        im = Image.open(src).convert('RGB'); x0, y0 = (box[0], box[1]) if box else (0, 0)
        crop = im.crop(box) if box else im
        res = det.detect(mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(np.asarray(crop))))
        if not res.face_landmarks:
            print('FACE', name, 'none'); continue
        w, h = crop.size
        pts = np.array([[p.x * w + x0, p.y * h + y0, p.z * w] for p in res.face_landmarks[0]])
        json.dump({'image': os.path.abspath(src), 'box': box, 'points': pts.round(2).tolist(), 'groups': GROUPS},
                  open(os.path.join(out, name + '.json'), 'w'))
        d = ImageDraw.Draw(im)
        for i, (x, y, _) in enumerate(pts):
            c = (255, 220, 0) if any(i in g for k, g in GROUPS.items() if k.startswith(('lip', 'eye'))) else (0, 255, 120)
            d.ellipse((x - 1.5, y - 1.5, x + 1.5, y + 1.5), fill=c)
        (im.crop(box) if box else im).save(os.path.join(out, name + '.jpg'), quality=90)
        print('FACE', name, 'points', len(pts))


if __name__ == '__main__':
    main()
