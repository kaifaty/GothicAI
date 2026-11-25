# A hairstyle of hair cards laid on the outer surface of the hair: slicked back into a ponytail.
#   python hair_cards.py cap.npz <out dir> [seed] [loose cards, 0]
# cap.npz is the surface of the hair as hair_cut.py measured it on the generator's model: radii around a centre in
# the skull, and the tail's axis with its radii. The hair of a slicked-back style runs over that surface to the
# knot, where the tail leaves the head; a card is a strip along such a line.
# - the base is strips from the hairline to the knot, side by side, so the whole surface is strands that run the
#   right way; over it locks, narrow greasy locks on top, single hairs at the edge, and a row of locks that start
#   under the rim, so the hairline is hair and not the edge of a shell;
# - under the cards lies a coarse shell 7 mm inside the surface, so no gap between cards opens onto the scalp;
# - the tail is two rings of cards around its axis, with a leather cord at the knot;
# - a card's normals are those of the hair's surface, not of its own plane: it is lit as a part of the volume;
# - the top layers and the tail have both faces in the mesh, the layers that lie on others only the outer one.
# UVs address the atlas of strand_atlas.py: eight tiles across, strands along v, the root at the top.
# Writes hair.npz: v, f (triangles), uv per face corner (v up, as Blender), n per vertex; metres, the head's frame.
import sys, os
import numpy as np
from scipy import ndimage

cap = np.load(sys.argv[1]); OUT = sys.argv[2]; rng = np.random.default_rng(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
LOOSE = int(sys.argv[4]) if len(sys.argv) > 4 else 0     # cards of single hairs at the edge: under an alpha test of one half they are wires
C, D, mask = cap['C'], float(cap['D']), cap['mask']; nt, nf = mask.shape
ix = ndimage.distance_transform_edt(~mask, return_distances=False, return_indices=True)
R = ndimage.gaussian_filter(cap['r'][ix[0], ix[1]], (1.0, 1.0), mode=('nearest', 'wrap'))      # radii everywhere, the mask says where hair is
soft = ndimage.gaussian_filter(mask.astype(np.float32), (1.0, 1.0), mode=('nearest', 'wrap'))


def grid_at(G, p):
    """Bilinear value of a grid at the direction of point p from the centre."""
    one = np.ndim(p) == 1; q = np.atleast_2d(p)-C; r = np.linalg.norm(q, axis=-1)
    th = np.degrees(np.arccos(np.clip(q[..., 0]/r, -1, 1))); ph = np.degrees(np.arctan2(q[..., 1], -q[..., 2])) % 360
    val = ndimage.map_coordinates(G, [np.clip((th-10)/D-0.5, 0, nt-1), ph/D-0.5], order=1, mode='grid-wrap'); u = q/r[..., None]
    return (val[0], u[0]) if one else (val, u)


def surf(p, lift=0.0):
    r, u = grid_at(R, p); return C+u*np.asarray(r+lift)[..., None]


def normal(p, e=0.004):
    """Outward normal of the hair's surface at p: from the surface points around it."""
    u = (p-C)/np.linalg.norm(p-C, axis=-1, keepdims=True); a = np.cross(u, [1.0, 0, 0]); a /= np.linalg.norm(a, axis=-1, keepdims=True)+1e-9; b = np.cross(u, a)
    n = np.cross(surf(p+a*e)-surf(p-a*e), surf(p+b*e)-surf(p-b*e)); n /= np.linalg.norm(n, axis=-1, keepdims=True)+1e-12
    return n*np.sign((n*u).sum(-1, keepdims=True))


tc, ax, e1, e2, tl, tr = cap['tail_c'], cap['tail_ax'], cap['tail_e1'], cap['tail_e2'], cap['tail_l'], cap['tail_r']
knot = surf(tc+tl[0]*ax)                                                 # where the tail leaves the head
th = np.radians(10+(np.arange(nt)+0.5)*D)[:, None]; ph = np.radians((np.arange(nf)+0.5)*D)[None, :]
dirs = np.stack([np.cos(th)+0*ph, np.sin(th)*np.sin(ph), -np.sin(th)*np.cos(ph)], -1); P = C+dirs*R[..., None]


def spread(points, n, first=None):
    """n points far from each other: each next one is the farthest from those taken."""
    i = [rng.integers(len(points)) if first is None else first]; d = np.linalg.norm(points-points[i[0]], axis=1)
    for _ in range(n-1):
        i.append(int(np.argmax(d))); d = np.minimum(d, np.linalg.norm(points-points[i[-1]], axis=1))
    return points[i]


def line(seed, length, step=0.010):
    """The way of a hair from seed over the surface towards the knot."""
    pts = [surf(seed)]
    while len(pts)*step < length:
        p = pts[-1]; to = knot-p; dist = np.linalg.norm(to)
        if dist < 0.014: break
        n = normal(p); t = to-n*(to@n); t /= np.linalg.norm(t)+1e-9; side = np.cross(n, t)
        for turn in (0, 25, -25, 50, -50, 75, -75):                       # straight to the knot, or round a notch of the hairline (an ear)
            a = np.radians(turn); q = surf(p+(np.cos(a)*t+np.sin(a)*side)*step)
            if grid_at(soft, q)[0] >= 0.12: break
        else: break                                                      # off the hair every way
        pts.append(q)
    return np.array(pts)


verts, norms, faces, uvs = [], [], [], []
def card(pts, nrm, width, lift, tiles, two_sided, dip=0.0):
    """A strip along pts: lifted by `lift` (root, tip) along the normals, its first point `dip` deeper, so the hair
    comes out of the skin; width is one number or one per point; mapped to one of the tiles."""
    k = len(pts); t = np.gradient(pts, axis=0); t /= np.linalg.norm(t, axis=1, keepdims=True)+1e-9
    b = np.cross(nrm, t); b /= np.linalg.norm(b, axis=1, keepdims=True)+1e-9
    f_ = np.linspace(0, 1, k)[:, None]; c = pts+nrm*(lift[0]+(lift[1]-lift[0])*f_); c[0] -= nrm[0]*dip
    width = np.broadcast_to(np.asarray(width, float), (k,))
    ti = rng.choice(tiles); flip = rng.uniform() < 0.5
    vs, v0 = 1.0, 1.0                  # the whole tile, the root at its top: a card cut short would end in a blunt edge, not in tips
    u0, u1 = (ti+0.01)/8, (ti+0.99)/8
    if flip: u0, u1 = u1, u0
    if np.cross(b[0], t[0])@nrm[0] < 0: b = -b                          # the first face looks out of the hair
    for side in ((0, 1) if two_sided else (0,)):
        base = len(verts)
        for i in range(k):
            for sgn, u in ((-1, u0), (1, u1)):
                verts.append(c[i]+sgn*b[i]*width[i]/2); norms.append(nrm[i]+sgn*0.25*b[i])   # a little round across the card
        for i in range(k-1):
            q = [base+2*i, base+2*i+1, base+2*i+3, base+2*i+2]; uq = [(u0, v0-vs*f_[i, 0]), (u1, v0-vs*f_[i, 0]), (u1, v0-vs*f_[i+1, 0]), (u0, v0-vs*f_[i+1, 0])]
            if side: q, uq = q[::-1], uq[::-1]
            for a in ((0, 1, 2), (0, 2, 3)):
                faces.append([q[j] for j in a]); uvs.append([uq[j] for j in a])


valid = P[mask]; rim = mask & ~ndimage.binary_erosion(mask)
edge = P[rim]; edge = edge[np.linalg.norm(edge-knot, axis=1) > 0.03]         # the hairline: the rim away from the knot
count = {}
# the base: strips from the hairline to the knot, side by side like meridians, so the whole surface is strands that
# run the right way. A strip is twice as wide as the step between strips, narrower only at the knot.
SPACE = 0.012; seeds = spread(edge, int(len(edge)*np.radians(D)*float(np.median(R[mask]))/SPACE)); made = 0
for sd in seeds:
    to = knot-sd; pts = line(sd+to/np.linalg.norm(to)*0.004, 1.0)          # from just inside the rim
    if len(pts) < 3: continue
    d = np.linalg.norm(pts-knot, axis=1); w = 2.0*SPACE*np.clip(d/0.05, 0.35, 1)        # the lines stay apart until close to the knot
    back = pts[0]-(pts[1]-pts[0])*0.5                                   # the root: half a step back, deep under the rim
    pts = np.vstack([back, pts]); w = np.r_[w[0], w]
    card(pts, normal(pts), w, (-0.0025, -0.0015), [0, 1, 2], False, dip=0.012); made += 1
count['base'] = made
for name, n, src, length, width, lift, tiles, two, dip in (
        ('mid', 80, valid, (0.07, 0.11), 0.019, (-0.0012, 0.0002), [3, 4, 5, 0], False, 0.003),
        ('top', 56, valid, (0.06, 0.09), 0.013, (0.0000, 0.0009), [6, 3, 4, 5], True, 0.002),
        ('line', 40, edge, (0.05, 0.08), 0.014, (-0.0008, 0.0008), [3, 4, 5, 6], True, 0.006),
        ('loose', LOOSE, edge, (0.03, 0.05), 0.010, (0.0003, 0.0015), [7], True, 0.006)):
    made = 0
    for sd in (spread(src, n) if n else []):
        pts = line(sd+rng.normal(0, 0.003, 3), rng.uniform(*length))
        if len(pts) < 3: continue
        card(pts, normal(pts), width*rng.uniform(0.85, 1.15), lift, tiles, two, dip=dip); made += 1
    count[name] = made

# the shell under the cards: every third node of the grid, 7 mm inside, of one dark colour
q = 3; sub = np.zeros_like(mask); sub[::q, ::q] = True; inner = ndimage.binary_dilation(mask, iterations=1)
gi = -np.ones(mask.shape, int); nodes = np.nonzero(sub & inner); gi[nodes] = len(verts)+np.arange(len(nodes[0]))
pn = P[nodes]; nn = normal(pn); verts.extend(pn-nn*0.007); norms.extend(nn); base_faces = len(faces)
for i in range(0, nt-q, q):
    for j in range(0, nf, q):
        j2 = (j+q) % nf if (j+q) % nf < nf else 0
        c4 = [gi[i, j], gi[i, j2], gi[i+q, j2], gi[i+q, j]]
        if min(c4) < 0: continue
        pc = np.array([verts[c] for c in c4]); out = np.cross(pc[1]-pc[0], pc[2]-pc[0])@(pc.mean(0)-C) > 0
        if not out: c4 = c4[::-1]
        uq = [(0.030+0.002*((a % 2)), 0.900+0.002*(a // 2)) for a in (0, 1, 3, 2)]   # one dark spot of dense strands: the shell is a backing, not hair
        for a in ((0, 1, 2), (0, 2, 3)):
            faces.append([c4[k] for k in a]); uvs.append([uq[k] for k in a])
count['shell'] = len(faces)-base_faces

# the tail: two rings of cards around the axis, and the cord
na = tr.shape[1]; ang = np.radians((np.arange(na)+0.5)/na*360)
def tail_ring(n, scale, tiles, width_k):
    for a0 in np.linspace(0, 2*np.pi, n, endpoint=False)+rng.uniform(0, 1):
        r = np.array([np.interp(a0 % (2*np.pi), ang, row, period=2*np.pi) for row in tr])*scale
        rad = np.cos(a0)*e1+np.sin(a0)*e2; pts = tc+tl[:, None]*ax+r[:, None]*rad
        card(pts, np.repeat(rad[None], len(pts), 0), 2*np.pi*np.median(r)/n*width_k, (0.0, 0.0), tiles, True)
tail_ring(12, 1.0, [0, 1, 2, 3, 4, 5], 2.0); tail_ring(7, 0.55, [0, 1, 2], 2.2)
r0 = float(np.median(tr[0]))+0.0015; base = len(verts); seg = 14
for i in range(seg):                                                    # the cord: a band around the tail's root
    a0 = 2*np.pi*i/seg; rad = np.cos(a0)*e1+np.sin(a0)*e2
    for l in (tl[0]+0.001, tl[0]+0.008):
        verts.append(tc+l*ax+r0*rad); norms.append(rad)
for i in range(seg):
    q4 = [base+2*i, base+2*((i+1) % seg), base+2*((i+1) % seg)+1, base+2*i+1]
    pc = np.array([verts[c] for c in q4]); cen = pc.mean(0)-tc; cen -= (cen@ax)*ax
    if np.cross(pc[1]-pc[0], pc[2]-pc[0])@cen < 0: q4 = q4[::-1]
    uq = [(7.1/8, 0.012), (7.9/8, 0.012), (7.9/8, 0.034), (7.1/8, 0.034)]
    for a in ((0, 1, 2), (0, 2, 3)):
        faces.append([q4[k] for k in a]); uvs.append([uq[k] for k in a])

v = np.array(verts); n = np.array(norms); n /= np.linalg.norm(n, axis=1, keepdims=True)
os.makedirs(OUT, exist_ok=True)
np.savez(os.path.join(OUT, 'hair.npz'), v=v, f=np.array(faces), uv=np.array(uvs), n=n)
print('CARDS', count, 'of %d base seeds' % len(seeds), '; verts %d, triangles %d' % (len(v), len(faces)))
