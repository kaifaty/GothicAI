# The eyes of a head as a material of the game (reports/content/2026-11-hero-head/eye-shader.md, "Способ" 4): the white
# of the eye warm and as light as the head's cheek skin times a share, not white; pinkish towards the corners; a dark
# ring of the limbus round the iris; and the roughness of the tear film for the eye shader (`eye: true`).
#   python eye_material.py <head.glb> <eye.png> <skin map> <out dir> [--share 1.0] [--depth]
# Input:
# - head.glb: the head as exported (export_test_head.py): its parts face, eye_L, eye_R give where the cheeks are in the
#   skin map and how each eyeball lies in the eye map (both eyes share one map, mirrored);
# - eye.png: the eye map (RGBA; its alpha marks the cornea shell export_test_head.py leaves out, and is kept);
# - skin map: the colour map of the head's skin as the game gets it (skin_game.png of skin_material.py, or its DDS);
# - --share: the sclera's linear luminance as a share of the cheek skin's (1.0: as light as the skin; with the lids'
#   shade of the eye shader the white then stays under the lit cheek, the acceptance of the eye shader);
# - --depth: how deep the iris lies under the cornea goes into the ORM's B (eye_cornea.py: the dome over each iris, its
#   depth down to the plane of the limbus over DEPTH_MAX_CM), for the refraction of the eye shader (main.frag) and the
#   export's --cornea, which raises the iris by the same eye_material.json. Without it B stays 0: no refraction.
# Output:
# - eye_game.png: the colour map: the sclera recoloured to a warm white of that luminance, its veins and shading kept
#   as they were relative to it, pinkish at the corners; the iris by the sclera's gain; the limbus darkened over 12 % of
#   the iris' diameter;
# - eye_orm.png: R = 1, G = roughness (the cornea over the iris EYE_ROUGH_CORNEA, the sclera EYE_ROUGH_SCLERA), B = 0 or,
#   with --depth, the iris' depth;
# - eye_material.json: what was measured (cheek skin, sclera before and after, the iris of each eye in the map: its pupil,
#   edge and the limbus' middle the cornea meets the white at).
import sys, os, json, struct
import numpy as np
from PIL import Image

a = sys.argv[1:]


def opt(name, default):
    if name in a:
        i = a.index(name); v = a[i+1]; del a[i:i+2]; return v
    return default


SHARE = float(opt('--share', '1.0'))
DEPTH = '--depth' in a
if DEPTH:
    a.remove('--depth')
GLB, EYE, SKIN, OUT = a[:4]
os.makedirs(OUT, exist_ok=True)
lin = lambda c: np.where(c > 0.04045, ((c+0.055)/1.055)**2.4, c/12.92)
srgb = lambda c: np.where(c > 0.0031308, 1.055*np.maximum(c, 1e-9)**(1/2.4)-0.055, c*12.92)
step = lambda x, a, b: np.clip((x-a)/(b-a), 0, 1)**2*(3-2*np.clip((x-a)/(b-a), 0, 1))
LUM = np.array([0.2126, 0.7152, 0.0722])
WARM = np.array([1.09, 0.99, 0.84])      # the sclera's colour per unit of luminance: sRGB ~(235, 225, 210), a warm white
PINK = np.array([1.06, 0.95, 0.95])      # at the corners, by the share CORNER of the ball's half-width across
CORNER = (0.35, 0.75)
LIMBUS = 0.12                            # the dark ring's width, of the iris' diameter (1-2 mm of 11.5)
LIMBUS_DARK = 0.45                       # the colour left at its darkest
EYE_ROUGH_CORNEA, EYE_ROUGH_SCLERA = 0.06, 0.10


def glb_parts(path):
    """POSITION and TEXCOORD_0 of every node's mesh, in the engine's frame of the head (cm: x up, y across, z forward)."""
    d = open(path, 'rb').read(); jl = struct.unpack_from('<I', d, 12)[0]; g = json.loads(d[20:20+jl])
    off = 20+jl; bl = struct.unpack_from('<I', d, off)[0]; b = d[off+8:off+8+bl]

    def acc(i):
        ac = g['accessors'][i]; v = g['bufferViews'][ac['bufferView']]; n = {'VEC2': 2, 'VEC3': 3}[ac['type']]
        o = v.get('byteOffset', 0)+ac.get('byteOffset', 0); st = v.get('byteStride', 4*n)
        raw = np.frombuffer(b[o:o+st*ac['count']], np.uint8).reshape(ac['count'], st)[:, :4*n].copy()
        return raw.view(np.float32).reshape(ac['count'], n).astype(np.float64)
    out = {}
    for node in g['nodes']:
        if 'mesh' in node:
            at = g['meshes'][node['mesh']]['primitives'][0]['attributes']
            p = acc(at['POSITION']); out[node['name']] = (np.stack([-100*p[:, 0], 100*p[:, 1], 100*p[:, 2]], 1), acc(at['TEXCOORD_0']))
    return out


def fit_sphere(p):
    A = np.c_[2*p, np.ones(len(p))]; x = np.linalg.lstsq(A, (p**2).sum(1), rcond=None)[0]
    return x[:3], np.sqrt(x[3]+x[:3] @ x[:3])


parts = glb_parts(GLB)
eye = np.asarray(Image.open(EYE).convert('RGBA')).astype(np.float64)/255; H, W = eye.shape[:2]
skin_img = Image.open(SKIN).convert('RGB'); skin = lin(np.asarray(skin_img).astype(np.float64)/255); SH, SW = skin.shape[:2]
face_p, face_uv = parts['face']

# ---- the cheek skin under each eye: the face's front-most vertices 2.4-3.4 cm under the ball's centre, its map there
cheek, balls = [], []
for side in ('eye_L', 'eye_R'):
    p, uv = parts[side]; c, r = fit_sphere(p); u = (p-c)/r
    front = u[:, 2] > 0.8
    A = np.c_[np.ones(front.sum()), u[front, 1], u[front, 0]]; sol = np.linalg.lstsq(A, uv[front], rcond=None)[0]
    balls.append({'centre_uv': sol[0], 'across': sol[1], 'up': sol[2], 'radius_cm': r})
    sel = (face_p[:, 0] > c[0]-3.4) & (face_p[:, 0] < c[0]-2.4) & (np.abs(face_p[:, 1]-c[1]) < 0.9)
    assert sel.any(), 'no cheek under ' + side
    k = np.nonzero(sel)[0]; k = k[face_p[k, 2] > face_p[k, 2].max()-0.5]
    for q in face_uv[k]:
        x, y = int(q[0]*SW), int(q[1]*SH); rr = max(2, SW//170)
        cheek.append(skin[max(0, y-rr):y+rr+1, max(0, x-rr):x+rr+1].reshape(-1, 3))
cheek = np.concatenate(cheek); cheek_lin = np.median(cheek, 0); cheek_lum = float(cheek_lin @ LUM)

# ---- each iris in the map: the pupil (darkest blob) near the ball's front, the iris' edge on its radial profile
Y, X = np.mgrid[0:H, 0:W]; U, V = (X+0.5)/W, (Y+0.5)/H
lum = lin(eye[..., :3]) @ LUM
opaque = eye[..., 3] > 0.5
iris_mask = np.zeros((H, W), bool); limbus = np.zeros((H, W)); rough = np.full((H, W), EYE_ROUGH_SCLERA)
corner = np.zeros((H, W)); ball_mask = np.zeros((H, W), bool); irises = []
for b in balls:
    c0 = b['centre_uv']; half = np.linalg.norm(b['across'])          # the ball's half-width across, in UV (u.y 0..1)
    d0 = np.hypot(U-c0[0], V-c0[1])
    near = (d0 < 0.12) & opaque
    pupil = np.argwhere(near & (lum < np.percentile(lum[near], 3)))
    pc = (pupil.mean(0)[::-1]+0.5)/[W, H]                             # (u, v)
    rr = np.hypot(U-pc[0], V-pc[1])
    bins = np.linspace(0, 0.2, 81); prof = np.array([np.median(lum[(rr >= bins[i]) & (rr < bins[i+1]) & opaque])
                                                     for i in range(80)])
    iris_l, scl_l = np.median(prof[12:30]), np.median(prof[52:70])   # 0.03-0.075: the iris, 0.13-0.175: the sclera
    dark = np.nonzero(prof[12:70] < 0.5*(iris_l+scl_l))[0]             # the iris' edge: the outermost ring still
    edge = bins[12+dark.max()+1]                                       # under half way to the sclera (its fibres are light)
    irises.append({'pupil_uv': pc.tolist(), 'iris_radius_uv': float(edge), 'limbus_uv': float(edge*(1+0.15*LIMBUS*2)),
                   'iris_lum': float(iris_l), 'sclera_lum': float(scl_l)})
    ball_mask |= (d0 < half*1.05) & opaque
    iris_mask |= rr < edge
    w = LIMBUS*2*edge                                                  # the ring's width in UV
    limbus = np.maximum(limbus, np.exp(-0.5*((rr-(edge+0.15*w))/(0.42*w))**2))
    rough = np.where(rr < edge+0.3*w, EYE_ROUGH_CORNEA, rough)
    across = ((U-c0[0])*b['across'][0]+(V-c0[1])*b['across'][1])/half**2   # u.y of the ball: -1..1 across
    corner = np.maximum(corner, step(np.abs(across), *CORNER)*(d0 < half*1.05))

# ---- the sclera: its median colour goes to the warm white of the cheek's luminance times the share; every texel keeps
# its ratio to the median (veins, shading). The iris takes the sclera's gain of luminance: its contrast to the white
# stays as painted (a light iris on a darkened white would be the brightest thing in the eye)
sclera = ball_mask & ~iris_mask & (limbus < 0.05)
src = lin(eye[..., :3]); med = np.median(src[sclera], 0)
target = WARM/(WARM @ LUM)*cheek_lum*SHARE
gain = float((target @ LUM)/(med @ LUM))
out = src.copy()
out[iris_mask] *= gain
wsc = (ball_mask & ~iris_mask).astype(np.float64)[..., None]
recol = src/med*target
recol = recol*(1+(PINK-1)*corner[..., None])
out = out*(1-wsc)+recol*wsc
out = out*(1-(1-LIMBUS_DARK)*limbus[..., None]*ball_mask[..., None])
res = np.concatenate([srgb(np.clip(out, 0, 1)), eye[..., 3:]], 2)
Image.fromarray((res*255+0.5).astype(np.uint8), 'RGBA').save(os.path.join(OUT, 'eye_game.png'))
depth = np.zeros((H, W))
if DEPTH:
    # each ball's triangles in the map, rho (off the eye's axis) interpolated over them, the dome's depth of it
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import eye_cornea
    glb = eye_cornea.Glb(GLB)
    for side, iris in zip(('eye_L', 'eye_R'), irises):
        P, _, uv, tris = eye_cornea.part(glb, side)
        cor = eye_cornea.Cornea(P, uv, iris['pupil_uv'], iris['limbus_uv'])
        rho = cor.rho(P)
        for t in tris:
            q = uv[t]*[W, H]-0.5
            x0, y0 = np.maximum(np.floor(q.min(0)).astype(int), 0); x1, y1 = np.minimum(np.ceil(q.max(0)).astype(int), [W-1, H-1])
            if x1 < x0 or y1 < y0:
                continue
            yy, xx = np.mgrid[y0:y1+1, x0:x1+1]
            m = np.array([[q[1, 0]-q[0, 0], q[2, 0]-q[0, 0]], [q[1, 1]-q[0, 1], q[2, 1]-q[0, 1]]])
            if abs(np.linalg.det(m)) < 1e-12:
                continue
            bc = np.linalg.solve(m, np.stack([xx.ravel()-q[0, 0], yy.ravel()-q[0, 1]]))
            inside = (bc >= -1e-6).all(0) & (bc.sum(0) <= 1+1e-6)
            r = rho[t[0]]*(1-bc[0]-bc[1])+rho[t[1]]*bc[0]+rho[t[2]]*bc[1]
            dep = cor.depth(r[inside])
            depth[yy.ravel()[inside], xx.ravel()[inside]] = np.maximum(depth[yy.ravel()[inside], xx.ravel()[inside]], dep)
        iris.update({'limbus_cm': cor.limbus, 'cornea_radius_cm': cor.rc, 'ball_radius_cm': cor.r,
                     'depth_centre_cm': float(cor.depth(np.array([0.0]))[0])})
    assert depth.max() < eye_cornea.DEPTH_MAX_CM, 'the iris lies deeper than the ORM holds'
orm = np.stack([np.ones((H, W)), rough, depth/(eye_cornea.DEPTH_MAX_CM if DEPTH else 1)], 2)
Image.fromarray((orm*255+0.5).astype(np.uint8), 'RGB').save(os.path.join(OUT, 'eye_orm.png'))
info = {'cheek_lin': cheek_lin.tolist(), 'cheek_lum': cheek_lum, 'share': SHARE,
        'sclera_before_lin': med.tolist(), 'sclera_before_lum': float(med @ LUM), 'sclera_after_lin': target.tolist(),
        'sclera_after_srgb': (srgb(target)*255).round().tolist(), 'iris_gain': gain, 'irises': irises,
        'depth_max_cm': eye_cornea.DEPTH_MAX_CM if DEPTH else 0}
json.dump(info, open(os.path.join(OUT, 'eye_material.json'), 'w'), indent=1)
print('EYE cheek lum %.3f, sclera %.3f -> %.3f (sRGB %s), iris gain %.2f, irises r %s' % (cheek_lum, med @ LUM, target @ LUM,
      info['sclera_after_srgb'], gain, [round(i['iris_radius_uv'], 3) for i in irises]))
