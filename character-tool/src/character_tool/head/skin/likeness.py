# The likeness sheet: the concept, the generator's model and our head at the same orthographic cameras as
# render_skin.py (front and profile, 0.36 m across 900 px, centred 3 cm under the eyes).
#   python likeness.py views.json params.json raw.npz front_ref.png profile_ref.png <dir with s_front/s_profile.png> out.jpg [--place place.json] [--tone tone.json]
# The concept is resampled by the scale of params.json; the model is drawn from its coloured vertices with a depth
# test. Rows: front, profile. Columns: concept, model, ours.
import sys, os, json
import numpy as np
from PIL import Image
from scipy import ndimage
import ref_model

OPT = ref_model.options(sys.argv)
VJ, PJ, RAW, FRONT, PROF, DIR, OUT = sys.argv[1:8]
V = json.load(open(VJ)); P = json.load(open(PJ)); raw = np.load(RAW)
eye = np.array(V['eye']); mpp = 0.0615/P['ipd_px']; N = 900; S = 0.36/N
rv, rc = ref_model.load(raw, P, eye, OPT)
rf = raw['faces']; rn = np.zeros_like(rv); fn = np.cross(rv[rf[:, 1]]-rv[rf[:, 0]], rv[rf[:, 2]]-rv[rf[:, 0]])
for k in range(3): np.add.at(rn, rf[:, k], fn)
rn /= np.linalg.norm(rn, axis=1, keepdims=True)+1e-12
BG = (122, 124, 128)

def model(ang, cy):
    r = np.radians(ang); right = np.array([np.cos(r), np.sin(r)]); view = np.array([-np.sin(r), np.cos(r)])
    q = rv[:, :2]-[0, cy]
    u = (q@right/S+N/2).astype(int); v = (N/2-(rv[:, 2]-(eye[2]-0.03))/S).astype(int); dep = q@view
    ok = (u >= 1) & (u < N-1) & (v >= 1) & (v < N-1)
    D = np.full((N, N), 9, np.float32); np.minimum.at(D, (v[ok], u[ok]), dep[ok]); D = ndimage.minimum_filter(D, 5)
    ok &= dep < D[np.clip(v, 0, N-1), np.clip(u, 0, N-1)]+0.003
    i = np.nonzero(ok)[0]; i = i[np.argsort(-dep[i])]
    img = np.zeros((N, N, 3), np.float32)+BG
    sh = (0.72+0.28*np.abs(rn[i, :2]@view))[:, None]                 # a soft light from the camera
    for du in (-1, 0, 1):
        for dv in (-1, 0, 1): img[v[i]+dv, u[i]+du] = rc[i]*sh
    return Image.fromarray(img.clip(0, 255).astype(np.uint8))

def concept(path, ex, ey, c0):
    # camera pixel (u, v) -> world (c0+(u-N/2)S, eye_z-0.03+(N/2-v)S) -> concept pixel
    k = S/mpp; a = (k, 0, ex+(c0-N/2*S)/mpp, 0, k, ey+(0.03-N/2*S)/mpp)
    return Image.open(path).convert('RGB').transform((N, N), Image.AFFINE, a, Image.BICUBIC, fillcolor=BG)

def ours(name):
    im = Image.open(os.path.join(DIR, f's_{name}.png')).convert('RGBA'); bg = Image.new('RGBA', im.size, BG+(255,))
    a = np.asarray(im); dark = a[..., :3].max(2) < 3                  # the render's black backdrop
    im = Image.fromarray(np.where(dark[..., None], np.array(BG+(255,), np.uint8), a))
    return Image.alpha_composite(bg, im).convert('RGB')

(fx, fy), (px, py) = P['front_eye'], P['profile_eye']
rows = [(concept(FRONT, fx, fy, 0.0), model(0, 0.0), ours('front')),
        (concept(PROF, px, py, 0.06), model(90, eye[1]+0.06), ours('profile'))]
sheet = Image.new('RGB', (3*N, 2*N))
for j, row in enumerate(rows):
    for i, im in enumerate(row): sheet.paste(im, (i*N, j*N))
sheet.resize((3*N*2//3, 2*N*2//3), Image.LANCZOS).save(OUT, quality=90)
