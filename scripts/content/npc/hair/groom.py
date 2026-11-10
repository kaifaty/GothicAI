# A hairstyle as strands: slicked back into a ponytail, the source for everything the game draws of it.
#   python groom.py cap.npz uv.npz groups.npz <out dir> [strands, 60000] [seed, 1] [--skin skin.png]
# cap.npz is the hair's outer surface and the tail of the generator's model (hair_cut.py); uv.npz and groups.npz are
# the head's mesh and its vertex groups (dump_uv.py): the scalp under the hair.
# - roots are spread evenly over the scalp inside the hairline; the hairline is the mask's edge with a slow noise.
#   The mask of hair_cut.py leaves out the hair in front of the ears (the ears' hollows are dark like hair), so with
#   --skin the scalp next to the mask that the skin map paints in the hair's colour grows hair too;
# - a hair is combed back over the head and, from some centimetres off, straight to the knot; where that way
#   leaves the hair (an ear), it follows the field of distance to the knot round it. Neither the shortest way nor
#   the straight way to the knot is combing: the first goes from the forehead round the side of the head, the
#   second fans out from the point opposite the knot, which is the middle of the hairline;
# - a hair lies between the scalp and the outer surface: it leaves the skin and rises to its own level;
# - hairs stick into locks, and locks into bigger locks: a hair is pulled to the hair that leads its lock;
# - at the knot the hairs pass the cord and fill the tail's measured shape; they end at different lengths;
# - a few hairs leave their lock and lift off, short hairs soften the hairline, every hair wavers a little.
# Writes groom.npz: pts (strands, K, 3) metres in the head's frame, root first; rad (root radius, metres);
# tint (-1..1, a lock's colour offset); kind (0 hair, 1 loose, 2 short at the hairline); cord (the tail's cord:
# centre, axis, radius, the two ends along the axis).
import sys, os
import numpy as np
from PIL import Image
from scipy import ndimage, sparse
from scipy.sparse.csgraph import dijkstra
from scipy.spatial import cKDTree

SKIN = None
if '--skin' in sys.argv:
    i = sys.argv.index('--skin'); SKIN = sys.argv[i+1]; del sys.argv[i:i+2]
cap = np.load(sys.argv[1]); uvd = np.load(sys.argv[2]); grp = np.load(sys.argv[3]); OUT = sys.argv[4]
N = int(sys.argv[5]) if len(sys.argv) > 5 else 60000; rng = np.random.default_rng(int(sys.argv[6]) if len(sys.argv) > 6 else 1)
K, H, STOP = 48, 0.0025, 0.022                    # points of a strand, the step over the head, where the way to the knot ends
C, D = cap['C'], float(cap['D']); DR = np.radians(D)
# The cap's grid leaves a hole of 10 degrees round its poles, and the poles are just over the ears: the grid grows
# 8 degrees there, with no measured hair; the skin map says whether hair grows in it.
PAD = 4; TH0 = 10-PAD*D; mask = np.pad(cap['mask'], ((PAD, PAD), (0, 0))); cap_r = np.pad(cap['r'], ((PAD, PAD), (0, 0)), mode='edge'); nt, nf = mask.shape
tc, ax, e1, e2, tl, tr = cap['tail_c'], cap['tail_ax'], cap['tail_e1'], cap['tail_e2'], cap['tail_l'], cap['tail_r']


def direction(g):
    """Unit direction from the centre for grid coordinates (..., 2): theta from +x, phi round x, 0 = down."""
    th = np.radians(TH0+(g[..., 0]+0.5)*D); ph = np.radians((g[..., 1]+0.5)*D)
    return np.stack([np.cos(th), np.sin(th)*np.sin(ph), -np.sin(th)*np.cos(ph)], -1)


def at(G, g):
    g = np.asarray(g, float); v = ndimage.map_coordinates(G, [np.atleast_1d(g[..., 0]), np.atleast_1d(g[..., 1])], order=1, mode='nearest')
    return v[0] if g.ndim == 1 else v


def coords(p):
    q = p-C; r = np.linalg.norm(q, axis=-1)
    th = np.degrees(np.arccos(np.clip(q[..., 0]/r, -1, 1))); ph = np.degrees(np.arctan2(q[..., 1], -q[..., 2])) % 360
    return np.stack([(th-TH0)/D-0.5, ph/D-0.5], -1), r


gi, gj = np.meshgrid(np.arange(nt), np.arange(nf), indexing='ij'); cells = np.stack([gi, gj], -1).astype(float); dirs = direction(cells)

# ---- the scalp: where the ray of every cell leaves the head's skin
co = uvd['co'].astype(np.float64); keep = grp['body'][uvd['tri']].all(1); keep &= co[uvd['tri']][:, :, 2].mean(1) > C[2]-0.16
tri = uvd['tri'][keep]; tuv = uvd['tuv'][keep]
near = ndimage.binary_dilation(mask, iterations=8); rs = np.zeros(mask.shape)
t0 = co[tri[:, 0]]; ea = co[tri[:, 1]]-t0; eb = co[tri[:, 2]]-t0; tv = C-t0; qv = np.cross(tv, ea)
rays = dirs[near]; hit = np.zeros(len(rays)); huv = np.zeros((len(rays), 2))
for s in range(0, len(rays), 96):                 # the first skin a ray meets: the farthest would be a shoulder behind the neck
    dr = rays[s:s+96]; pv = np.cross(dr[:, None], eb[None]); det = (ea[None]*pv).sum(2); det = np.where(np.abs(det) < 1e-12, 1e-12, det)
    u = (tv[None]*pv).sum(2)/det; w = (dr[:, None]*qv[None]).sum(2)/det; t = (eb[None]*qv[None]).sum(2)/det
    t = np.where((u >= 0) & (w >= 0) & (u+w <= 1) & (t > 0.02), t, np.inf); k = t.argmin(1); r_ = np.arange(len(dr))
    hit[s:s+96] = t[r_, k]; uu, ww = u[r_, k][:, None], w[r_, k][:, None]
    huv[s:s+96] = tuv[k, 0]*(1-uu-ww)+tuv[k, 1]*uu+tuv[k, 2]*ww
rs[near] = np.where(np.isfinite(hit), hit, 0); known = near & (rs > 0)
ix = ndimage.distance_transform_edt(~known, return_distances=False, return_indices=True); rs = rs[ix[0], ix[1]]
grown = np.zeros_like(mask); ear = np.zeros_like(mask)
if SKIN:                                          # scalp painted as hair next to the mask, not an ear's hollow
    im = np.asarray(Image.open(SKIN).convert('RGB')).astype(int); hh, ww_ = im.shape[:2]
    c = im[np.clip(((1-huv[:, 1])*hh).astype(int), 0, hh-1), np.clip((huv[:, 0]*ww_).astype(int), 0, ww_-1)]
    hairy = np.zeros_like(mask); hairy[near] = (c[:, 0]-c[:, 2] < 50) & (c[:, 1] > 0.70*c[:, 0]) & (c.max(1) < 150) & np.isfinite(hit)
    sp = C+dirs*rs[..., None]; ear = (np.abs(sp[..., 0]) > 0.084) & (sp[..., 2] < C[2]-0.005)
    grown = ndimage.binary_opening(hairy & ~ear & ~mask & ndimage.binary_dilation(mask, iterations=7), iterations=1)
    grown = ndimage.binary_propagation(ndimage.binary_dilation(mask) & grown, mask=grown)   # only what touches the hair
bare = mask & (rs > cap_r+0.004)                # the measured hair hangs clear of the skin there: nothing to root in
rh = np.where(mask, cap_r, rs+0.004); mask = mask | grown      # off the measured hair the layer lies on the skin
rh = ndimage.gaussian_filter(rh, (1.0, 1.0), mode=('nearest', 'wrap'))
r_in = ndimage.gaussian_filter(rs, 0.7, mode=('nearest', 'wrap'))+0.0003
# The layer grows from the hairline inwards: at the line hair lies on the skin, whatever the measured surface says
# there (the head is fitted to the bald model, and its forehead is not the full model's to the millimetre).
grow = 0.0015+0.45*ndimage.distance_transform_edt(mask)*float(np.median(rs[mask]))*DR
r_out = r_in+np.clip(rh-r_in, 0.0015, grow)
thick = (rh-rs)[mask & ~bare]

# ---- the hairline: the mask's edge, moved in and out by a slow noise
soft = ndimage.gaussian_filter(mask.astype(np.float32), 0.8, mode=('nearest', 'wrap'))
edge_noise = ndimage.gaussian_filter(rng.normal(0, 1, mask.shape), 1.2, mode=('nearest', 'wrap')); edge_noise *= 0.10/edge_noise.std()
inside = soft-0.5-edge_noise                       # > 0 where hair grows; ~0.3 a centimetre inside the line
inside = np.where(ear, -0.6, inside)               # an ear is not a place for hair to lie on
rootable = ndimage.gaussian_filter(1.0-bare, 0.8, mode=('nearest', 'wrap'))

# ---- the distance to the knot over the hair's surface
knot = tc+tl[0]*ax; kg, _ = coords(knot); knot3 = C+direction(kg)*at(r_out, kg)
region = ndimage.binary_dilation(mask, iterations=1); idx = -np.ones(mask.shape, int); idx[region] = np.arange(region.sum())
P = C+dirs*r_out[..., None]; rows, cols, wts = [], [], []
for di, dj in ((0, 1), (1, 0), (1, 1), (1, -1), (1, 2), (2, 1), (1, -2), (2, -1)):
    a = idx[:nt-di]; b = np.roll(idx, -dj, axis=1)[di:]; ok = (a >= 0) & (b >= 0)
    d = np.linalg.norm(P[:nt-di]-np.roll(P, -dj, axis=1)[di:], axis=-1)
    rows.append(a[ok]); cols.append(b[ok]); wts.append(d[ok])
gr = sparse.coo_matrix((np.concatenate(wts), (np.concatenate(rows), np.concatenate(cols))), shape=(region.sum(),)*2)
src = idx[tuple(np.argwhere(region)[np.argmin(np.linalg.norm(P[region]-knot3, axis=1))])]
T = np.full(mask.shape, np.inf); T[region] = dijkstra(gr, directed=False, indices=src)
fin = np.isfinite(T); wsum = ndimage.gaussian_filter(fin.astype(float), 1.0, mode=('nearest', 'wrap'))
Ts = ndimage.gaussian_filter(np.where(fin, T, 0), 1.0, mode=('nearest', 'wrap'))/np.maximum(wsum, 1e-6)
dist, ix = ndimage.distance_transform_edt(~fin, return_indices=True)
T = np.where(fin, Ts, Ts[ix[0], ix[1]]+dist*0.014)   # off the hair the field rises steeply: a hair that strays turns back
Tp = np.pad(T, ((0, 0), (1, 1)), mode='wrap'); dTi = np.gradient(T, axis=0); dTj = (Tp[:, 2:]-Tp[:, :-2])/2


BACK = np.array([0.0, 1.0, -0.25]); BACK /= np.linalg.norm(BACK)
def combed(g):
    """The step of grid coordinates that goes H metres along the hair: to the knot, or round what is in the way."""
    r = at(r_out, g); th = np.radians(TH0+(g[..., 0]+0.5)*D); ph = np.radians((g[..., 1]+0.5)*D); st = np.maximum(np.sin(th), 0.03)
    a = -at(dTi, g)/(r*DR); b = -at(dTj, g)/(r*st*DR); n = np.sqrt(a*a+b*b)+1e-9; a, b = a/n, b/n   # down the distance field
    u = direction(g); to = knot3-(C+u*r[..., None]); far = np.linalg.norm(to, axis=-1, keepdims=True)
    to -= u*(to*u).sum(-1, keepdims=True); to /= np.linalg.norm(to, axis=-1, keepdims=True)+1e-9
    back = BACK-u*(u@BACK)[..., None]; back /= np.linalg.norm(back, axis=-1, keepdims=True)+1e-9
    to = to+(back-to)*np.clip((far-0.06)/0.08, 0, 1); to /= np.linalg.norm(to, axis=-1, keepdims=True)+1e-9
    ka = (to*np.stack([-np.sin(th), np.cos(th)*np.sin(ph), -np.cos(th)*np.cos(ph)], -1)).sum(-1); kb = (to*np.stack([0*ph, np.cos(ph), np.sin(ph)], -1)).sum(-1)
    ahead = g+4*np.stack([ka*H/(r*DR), kb*H/(r*st*DR)], -1); w = np.clip(at(inside, ahead)/0.04, 0, 1)   # 1: the straight way stays on the hair
    a, b = w*ka+(1-w)*a, w*kb+(1-w)*b; n = np.sqrt(a*a+b*b)+1e-9
    return np.stack([a/n*H/(r*DR), b/n*H/(r*st*DR)], -1)


# ---- roots: even over the scalp's area, inside the hairline
area = (r_in**2*np.sin(np.radians(TH0+(gi+0.5)*D))); amax = area[region].max(); lo = np.argwhere(region).min(0); hi = np.argwhere(region).max(0)
def roots(n, test):
    out = np.zeros((0, 2))
    while len(out) < n:
        g = rng.uniform(lo-0.5, hi+0.5, (4*n, 2)); ok = test(at(inside, g)) & (at(rootable, g) > 0.6) & (rng.uniform(0, amax, len(g)) < at(area, g))
        out = np.vstack([out, g[ok]])
    return out[:n]
N_SHORT = N//25                                    # short hairs of the hairline
g0 = np.vstack([roots(N, lambda v: v > 0), roots(N_SHORT, lambda v: (v > 0) & (v < 0.22))])
kind = np.r_[np.zeros(N, int), np.full(N_SHORT, 2)]; kind[:N][rng.uniform(size=N) < 0.025] = 1
n_all = len(g0); Troot = at(T, g0)

# ---- the way of every hair to the knot
S = int(1.35*Troot.max()/H)+3; pos = np.zeros((n_all, S+1, 2), np.float32); pos[:, 0] = g0; steps = np.zeros(n_all, int)
for k in range(S):
    g = pos[:, k].astype(float); go = np.linalg.norm(C+direction(g)*at(r_out, g)[:, None]-knot3, axis=1) > STOP
    mid = g+0.5*combed(g); nxt = g+combed(mid)
    pos[:, k+1] = np.where(go[:, None], nxt, g); steps += go
unreached = int((steps >= S).sum())
off = at(inside, pos.reshape(-1, 2).astype(float)).reshape(n_all, S+1) < -0.15; first = np.where(off.any(1), off.argmax(1), S+1)
cut = first <= steps; kind[cut] = 2; steps[cut] = np.maximum(first[cut]-1, 1)   # a hair that leaves the hair (onto an ear) ends there
kind[steps >= S] = 2                               # a hair that found no way to the knot stays short where it grows
side = (at(ndimage.gaussian_filter(grown.astype(float), 0.6), g0) > 0.5) & ((C+direction(g0)*at(r_in, g0)[:, None])[:, 2] < C[2]-0.008)
kind[side] = 2                                     # and so does the hair in front of the ears, below their tops
root3 = C+direction(g0)*at(r_in, g0)[:, None]


def clump(pos, members, share, strength, free):
    """Pull hairs to the hair that leads their lock: `share` of the members lead, the pull grows over `free` metres."""
    lead = members[rng.choice(len(members), max(1, int(len(members)*share)), replace=False)]
    L = lead[cKDTree(root3[lead]).query(root3[members])[1]]
    m = (steps[L]-steps[members])[:, None]+np.arange(S+1)[None]          # the leader's step with as far to go to the knot
    mc = np.clip(m, 0, steps[L][:, None]); f0 = np.floor(mc).astype(int); f1 = np.minimum(f0+1, S); w = (mc-f0)[..., None]
    g = pos[L[:, None], f0]*(1-w)+pos[L[:, None], f1]*w
    ramp = lambda x: np.clip(x, 0, 1)**2*(3-2*np.clip(x, 0, 1))
    c = strength[:, None]*ramp(np.arange(S+1)[None]*H/free)*ramp(m*H/0.008)
    pos[members] = pos[members]+(g-pos[members])*c[..., None]
    return L


hair = np.nonzero(kind < 2)[0]
big = clump(pos, hair, 1/70, rng.uniform(0.45, 0.85, len(hair)), 0.035)   # locks about a centimetre wide
small = clump(pos, hair, 1/12, rng.uniform(0.75, 0.97, len(hair)), 0.012)  # a dozen hairs stuck together: unwashed hair
lock_of = np.arange(n_all); lock_of[hair] = big; fine_of = np.arange(n_all); fine_of[hair] = small
u_lock = rng.uniform(size=n_all); n_lock = rng.normal(size=n_all)
lane = np.clip(0.12+0.88*u_lock[lock_of]+0.10*n_lock[fine_of]+rng.normal(0, 0.03, n_all), 0.03, 1.0)   # a lock lies at one level of the layer
n_fine = rng.normal(size=n_all); tint = np.clip(0.5*n_lock[lock_of]+0.75*n_fine[fine_of]+0.25*rng.normal(size=n_all), -2, 2)/2

# loose hairs stop early and lift off; the short hairs of the hairline are a centimetre or two
loose = kind == 1; steps[loose] = (steps[loose]*rng.uniform(0.25, 0.7, loose.sum())).astype(int)
short = kind == 2; steps[short] = np.minimum(steps[short], rng.integers(3, 10, short.sum()))
lift_tip = np.where(loose, rng.uniform(0.0015, 0.006, n_all), np.where(short, rng.uniform(0.0005, 0.004, n_all), 0.0))

# ---- into space: between the scalp and the outer surface
kk = np.arange(S+1)[None]; g = pos.astype(float); ri = at(r_in, g); ro = at(r_out, g)
depth = lane[:, None]*(1-np.exp(-kk*H/0.030)); fr = np.clip(kk/np.maximum(steps[:, None], 1), 0, 1)
rad3 = ri+depth*(ro-ri)+lift_tip[:, None]*fr**1.5; rad3[:, 0] = ri[:, 0]-0.0012
surf = (C+direction(g)*rad3[..., None]).astype(np.float32)

# ---- the tail: a hair keeps the side it came from and fills the measured shape
tr = np.minimum(tr, 1.25*np.median(tr, axis=1, keepdims=True))          # the tail's root merges with the hair of the head: no wings
na = tr.shape[1]; ang = (np.arange(na)+0.5)/na*2*np.pi; l0, l1 = tl[0], tl[-1]
nk = (knot3-C)/np.linalg.norm(knot3-C); up = -ax-(-ax@nk)*nk; up /= np.linalg.norm(up)
ring = surf[np.arange(n_all), steps].astype(float)-knot3; sx = ring[:, 0]; cy = ring@up; nrm = np.sqrt(sx*sx+cy*cy)+1e-9
a_t = np.arctan2(-sx/nrm, -cy/nrm)+rng.normal(0, 0.25, n_all)            # from the crown: the outer side; from the nape: the neck's side
q_t = np.sqrt(rng.uniform(0.02, 1, n_all))
l_end = l0+(l1-l0)*(1-0.7*rng.uniform(size=n_all)**1.7)+rng.normal(0, 0.003, n_all)
TN = 16; ut = np.linspace(0, 1, TN)**1.3; lt = l0+ut[None]*(l_end-l0)[:, None]
tri_ = np.stack([np.interp(a_t, ang, row, period=2*np.pi) for row in tr], 1)   # (hairs, lengths)
rt = np.stack([np.interp(lt[i], tl, tri_[i]) for i in range(n_all)])*q_t[:, None]
squeeze = 1-0.35*np.exp(-((lt-l0-0.005)/0.006)**2)                       # the cord pulls the hairs together
wave = 0.0030*np.clip((lt-l0)/(l1-l0), 0, 1)*np.sin((lt-l0)/0.045*2*np.pi+u_lock[lock_of][:, None]*6.28)
radv = np.cos(a_t)[:, None]*e1[None]+np.sin(a_t)[:, None]*e2[None]; tang = np.cross(ax[None], radv)
tail = tc[None, None]+lt[..., None]*ax[None, None]+(rt*squeeze)[..., None]*radv[:, None]+wave[..., None]*tang[:, None]

# ---- one curve of K points per hair
ui = np.linspace(0, 1, K); pts = np.zeros((n_all, K, 3), np.float32)
for i in range(n_all):
    c = surf[i, :steps[i]+1] if kind[i] else np.vstack([surf[i, :steps[i]+1], tail[i]])
    if len(c) < 2: c = np.vstack([c, c+nk*0.003])
    s = np.r_[0, np.cumsum(np.linalg.norm(np.diff(c, axis=0), axis=1))]; s /= s[-1]
    pts[i] = np.stack([np.interp(ui, s, c[:, j]) for j in range(3)], 1)
for _ in range(2):                                                       # round the turn into the tail
    pts[:, 1:-1] = 0.25*pts[:, :-2]+0.5*pts[:, 1:-1]+0.25*pts[:, 2:]

# every hair wavers a little, more to its tip
NC = 9; ctrl = rng.normal(0, 1, (n_all, NC, 3)).astype(np.float32); ctrl[:, 0] = 0
wm = np.zeros((K, NC), np.float32); x = ui*(NC-1); i0 = np.minimum(x.astype(int), NC-2); wm[np.arange(K), i0] = 1-(x-i0); wm[np.arange(K), i0+1] = x-i0
amp = (0.00010+0.00045*ui)[None]*np.where(kind == 1, 3.0, np.where(kind == 2, 1.6, 1.0))[:, None]
pts += np.einsum('kc,ncj->nkj', wm, ctrl)*amp[..., None]
gq, rq = coords(pts.astype(float)); floor = at(r_in, gq)+0.0002; low = (rq < floor) & (np.arange(K)[None] > 0)   # never under the skin
pts = np.where(low[..., None], C+(pts-C)/rq[..., None]*floor[..., None], pts).astype(np.float32)

rad = np.where(kind == 2, 0.000028, 0.00004)*rng.uniform(0.85, 1.15, n_all)
r_c = float(np.median(tr[1]))
os.makedirs(OUT, exist_ok=True)
np.savez(os.path.join(OUT, 'groom.npz'), pts=pts, rad=rad.astype(np.float32), tint=tint.astype(np.float32), kind=kind,
         cord=np.r_[tc, ax, r_c, l0+0.002, l0+0.010])
length = np.linalg.norm(np.diff(pts, axis=1), axis=2).sum(1)
print('GROOM hairs %d (+%d loose, +%d short), locks %d of %d; layer over the scalp mm: median %.1f, 10%% %.1f, 90%% %.1f; '
      'length cm: median %.1f, max %.1f; tail %.0f mm, cord radius %.1f mm; did not reach the knot %d' % (
          (kind == 0).sum(), loose.sum(), short.sum(), len(np.unique(big)), len(np.unique(small)),
          1000*np.median(thick), 1000*np.percentile(thick, 10), 1000*np.percentile(thick, 90),
          100*np.median(length[kind == 0]), 100*length.max(), 1000*(l1-l0), 1000*r_c, unreached))
