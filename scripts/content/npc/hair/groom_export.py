# Strands for the game: the grooms of groom.py and beard.py as one file the engine draws as ribbons.
#   python groom_export.py frame.json uv.npz groups.npz <out dir> --hair groom.npz [--beard beard_groom.npz]
#       [--strands 40000] [--seed 1] [--material OWN_HERO_HEAD_STRANDS.TGA]
# frame.json is written by export_test_head.py next to the head: the human's frame (metres) -> the head's frame in
# the engine (cm). uv.npz and groups.npz are the registered head: its skin stands in the way of light.
# What is done here, because the engine only draws:
# - the count: the long hairs are shuffled and the first --strands of them kept, each wider by the share dropped,
#   so the hair covers as much; a strand keeps 16 points (short hairs 6, the beard 8, stubble 3);
# - the normal of the hair's surface at every point: against the gradient of the blurred density of hair and head.
#   The light passes shade hair by it as by the surface of the hairstyle;
# - self-shadowing: how much light reaches a point through the hair that lies over it. Five rays go from the point,
#   along that normal and 40 degrees aside, and count the hairs on their way; a grid cannot say it, a millimetre
#   of combed hair already stops half of the light. It is stored per point and multiplies the light in the game;
# - the colour ramp of the hair as a strip (hair_ramp.png): the game looks a strand's colour up by its tint.
# Writes <out dir>/head.groom and hair_ramp.png. The file, little endian: 'GRM1', uint32 groups, char[64] material
# name; per group uint32 points in a strand, uint32 strands, then per point 32 bytes: float pos[3] cm, float
# tangent[3] root to tip, float half width cm, uint32 = normal octahedral x | y << 8 | visibility << 16 | tint << 24;
# then one byte per strand (a random number, padded to 4 bytes). The order in a group is random: the first share of
# a group is that share of the hair.
import sys, os, json, struct
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree

a = sys.argv[1:]


def opt(name, default=None):
    if name not in a: return default
    i = a.index(name); v = a[i+1]; del a[i:i+2]; return v


HAIR, BEARD = opt('--hair'), opt('--beard'); KEEP = int(opt('--strands', 40000)); SEED = int(opt('--seed', 1))
MATERIAL = opt('--material', 'OWN_HERO_HEAD_STRANDS.TGA')
FRAME, UV, GR, OUT = a[:4]; rng = np.random.default_rng(SEED)
M = np.array(json.load(open(FRAME))['matrix']); A, b = M[:, :3], M[:, 3]; SCALE = abs(np.linalg.det(A))**(1/3)   # cm in a metre of the head
VOX, EXT = 0.0025, 0.4                    # the grid; how much of a hair's width stops light (a hair lets a part through)


def resample(p, k):
    """Strands (n, K, 3) at k points even along their length."""
    seg = np.linalg.norm(np.diff(p, axis=1), axis=2); s = np.concatenate([np.zeros((len(p), 1)), np.cumsum(seg, 1)], 1)
    t = np.linspace(0, 1, k)[None]*s[:, -1:]; out = np.empty((len(p), k, 3))
    idx = np.clip((s[:, None, :] <= t[:, :, None]).sum(2)-1, 0, p.shape[1]-2); ar = np.arange(len(p))[:, None]
    f = ((t-s[ar, idx])/np.maximum(s[ar, idx+1]-s[ar, idx], 1e-12))[..., None]
    return p[ar, idx]*(1-f)+p[ar, idx+1]*f


# ---- the strands of the game: groups of (points, strands)
groups = []; dense = []                   # dense: every hair as it is, for the density
G = np.load(HAIR); pts, rad, tint, kind = G['pts'].astype(np.float64), G['rad'], G['tint'], G['kind']
dense.append((pts, rad))
long_ = np.nonzero(kind < 2)[0]; rng.shuffle(long_); keep = long_[:KEEP]; wide = min(len(long_)/max(len(keep), 1), 3.0)
short = np.nonzero(kind == 2)[0]; rng.shuffle(short); short = short[:max(int(len(short)*len(keep)/len(long_)), 1)]
groups.append((resample(pts[keep], 16), rad[keep]*wide, tint[keep]))
groups.append((resample(pts[short], 6), rad[short]*wide, tint[short]))
if BEARD:
    Bd = np.load(BEARD); bp, br, bt, bk = Bd['pts'].astype(np.float64), Bd['rad'], Bd['tint'], Bd['kind']
    dense.append((bp, br))
    for sel, k in ((bk != 5, 8), (bk == 5, 3)):
        i = np.nonzero(sel)[0]; rng.shuffle(i); groups.append((resample(bp[i], k), br[i], bt[i]))

# ---- the density of hair and the head in a grid (metres, the human's frame)
allp = np.concatenate([p.reshape(-1, 3) for p, _ in dense]); lo = allp.min(0)-0.03; hi = allp.max(0)+0.03
dim = np.ceil((hi-lo)/VOX).astype(int); rho = np.zeros(dim, np.float32)
for p, r in dense:
    mid = 0.5*(p[:, 1:]+p[:, :-1]); ln = np.linalg.norm(p[:, 1:]-p[:, :-1], axis=2)*(2*r)[:, None]
    ix = np.clip(((mid.reshape(-1, 3)-lo)/VOX).astype(int), 0, dim-1)
    np.add.at(rho, (ix[:, 0], ix[:, 1], ix[:, 2]), (ln.reshape(-1)/VOX**3).astype(np.float32))
rho = ndimage.gaussian_filter(rho, 0.6)                                      # 1/m: the share of light a metre of this hair stops
d = np.load(UV); g = np.load(GR); co, no, tri = d['co'].astype(np.float64), d['no'].astype(np.float64), d['tri']
st = tri[g['body'][tri].all(1)]; st = st[(co[st].mean(1) > lo-0.02).all(1) & (co[st].mean(1) < hi+0.02).all(1)]
ar = np.linalg.norm(np.cross(co[st[:, 1]]-co[st[:, 0]], co[st[:, 2]]-co[st[:, 0]]), axis=1)/2
k = rng.choice(len(st), int(ar.sum()*1.5e6), p=ar/ar.sum()); u = rng.random((len(k), 2)); f = u.sum(1) > 1; u[f] = 1-u[f]
w = np.c_[1-u.sum(1), u]; sp = (co[st[k]]*w[..., None]).sum(1); sn = (no[st[k]]*w[..., None]).sum(1)
gx = np.stack(np.meshgrid(*[np.arange(n) for n in dim], indexing='ij'), -1).reshape(-1, 3)
vc = lo+(gx+0.5)*VOX; dd, ii = cKDTree(sp).query(vc, workers=-1)
inside = (((vc-sp[ii])*sn[ii]).sum(1) < -0.0005).reshape(dim)                # behind the skin: the head (open at the neck: it reaches down)
solid = ndimage.binary_fill_holes(inside)
shape = ndimage.gaussian_filter(rho*EXT/400.0+solid.astype(np.float32), 0.006/VOX)   # the blurred body of head and hair
grad = np.stack(np.gradient(shape, VOX), -1)

sample = lambda F, p: ndimage.map_coordinates(F, ((p-lo)/VOX-0.5).T, order=1, mode='nearest')

# Every hair as points that stand for the same projected area each: what lies over a point is counted by them.
# A grid cannot say it: a millimetre of this hair already stops half of the light.
AREA, RB = 0.25e-6, 0.002                 # m2 a point stands for; the radius of the balls a ray is counted with
cloud = []
for p, r in dense:
    ln = np.linalg.norm(np.diff(p, axis=1), axis=2).sum(1); cnt = np.maximum(np.round(ln*2*r/AREA), 1).astype(int)
    for c in np.unique(cnt):
        m = cnt == c; cloud.append(resample(p[m], 2*c+1)[:, 1::2].reshape(-1, 3))
cloud = cKDTree(np.concatenate(cloud))
TILT = np.radians(40)


def shade(p):
    """Normal of the hair's surface and how much light reaches every point through the hair over it: (n, 3), (n,)."""
    n = -np.stack([sample(grad[..., c], p) for c in range(3)], 1); n /= np.linalg.norm(n, axis=1, keepdims=True)+1e-12
    e1 = np.cross(n, [0.0, 0.0, 1.0]); bad = np.linalg.norm(e1, axis=1) < 0.1; e1[bad] = np.cross(n[bad], [1.0, 0.0, 0.0])
    e1 /= np.linalg.norm(e1, axis=1, keepdims=True); e2 = np.cross(n, e1)
    rays = [(n, 2.0)]+[(np.cos(TILT)*n+np.sin(TILT)*(np.cos(q)*e1+np.sin(q)*e2), 1.0) for q in np.arange(4)*np.pi/2]
    vis = np.zeros(len(p)); wsum = 0.0
    for dv, wd in rays:
        tau = np.zeros(len(p))
        for j in range(6):                 # balls 4/3 of a radius apart hold as much as the ray's cylinder
            tau += cloud.query_ball_point(p+dv*(RB*(1.2+j*4/3)), RB, return_length=True, workers=-1)
        vis += wd*np.exp(-tau*AREA/(np.pi*RB*RB)*EXT); wsum += wd
    return n, vis/wsum


def octa(n):
    n = n/np.abs(n).sum(1, keepdims=True); x, y = n[:, 0].copy(), n[:, 1].copy(); neg = n[:, 2] < 0
    x[neg], y[neg] = (1-np.abs(n[neg, 1]))*np.sign(n[neg, 0]+1e-30), (1-np.abs(n[neg, 0]))*np.sign(n[neg, 1]+1e-30)
    return np.clip(np.round((np.stack([x, y], 1)*0.5+0.5)*255), 0, 255).astype(np.uint32)


# ---- the file
out = [b'GRM1', struct.pack('<I', len(groups)), MATERIAL.encode().ljust(64, b'\0')]; total = 0; seen = []
for p, r, t in groups:
    n, k = p.shape[:2]; flat = p.reshape(-1, 3)
    tan = np.gradient(p, axis=1); tan /= np.linalg.norm(tan, axis=2, keepdims=True)+1e-12
    nrm, vis = shade(flat)
    tflat = tan.reshape(-1, 3); nrm -= tflat*(nrm*tflat).sum(1, keepdims=True); nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)+1e-12
    taper = np.clip((1-np.linspace(0, 1, k))/0.3, 0.35, 1.0)                 # thinner over the last third
    hw = (r[:, None]*taper[None]*SCALE).reshape(-1)
    en = nrm @ A.T; en /= np.linalg.norm(en, axis=1, keepdims=True); o = octa(en)
    et = tflat @ A.T; et /= np.linalg.norm(et, axis=1, keepdims=True)
    tb = np.clip(np.round((np.repeat(t, k)*0.5+0.5)*255), 0, 255).astype(np.uint32)
    vb = np.clip(np.round(vis*255), 0, 255).astype(np.uint32)
    rec = np.zeros(n*k, dtype=[('pos', '<f4', 3), ('tan', '<f4', 3), ('hw', '<f4'), ('bits', '<u4')])
    rec['pos'] = flat @ A.T+b; rec['tan'] = et; rec['hw'] = hw; rec['bits'] = o[:, 0] | (o[:, 1] << 8) | (vb << 16) | (tb << 24)
    ids = rng.integers(0, 256, n, dtype=np.uint8).tobytes()
    out += [struct.pack('<II', k, n), rec.tobytes(), ids.ljust((n+3)//4*4, b'\0')]; total += n*k; seen.append(vis)
    print('group: %6d strands of %2d points; visibility 10%% %.2f, median %.2f, 90%% %.2f' % (n, k, *np.percentile(vis, [10, 50, 90])))
open(os.path.join(OUT, 'head.groom'), 'wb').write(b''.join(out))

# ---- the colour ramp: the concept's hair from shade over hair to the golden strands. groom_render.py has the same
# stops darker: there they are the pigment of Cycles' hair, which light scattered inside the hair makes lighter;
# here they are the colour the hair is seen in
stops = [(0.0, (30, 21, 14)), (0.10, (30, 21, 14)), (0.55, (84, 56, 34)), (0.95, (175, 125, 70)), (1.0, (175, 125, 70))]
x = np.linspace(0, 1, 256); ramp = np.stack([np.interp(x, [s[0] for s in stops], [s[1][c] for s in stops]) for c in range(3)], 1)
Image.fromarray(np.repeat(ramp[None], 8, 0).round().astype(np.uint8)).save(os.path.join(OUT, 'hair_ramp.png'))
print('GROOM %d strands, %d points, %d triangles; long hairs kept %d of %d, %.2f times wider; %.1f MB' % (
    sum(len(p) for p, _, _ in groups), total, sum(2*len(p)*(p.shape[1]-1) for p, _, _ in groups), len(keep), len(long_), wide,
    os.path.getsize(os.path.join(OUT, 'head.groom'))/1e6))
