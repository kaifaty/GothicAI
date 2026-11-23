"""Landmarks of the ears for the wrap, found without hand picks: the rim of each ear on the dense model and on the template.

  python ear_points.py <reg dir> <lm_fixed in> <lm_fixed out> [check.jpg] [--mask ears.npz] [--keep-hand]

<reg dir>: views.json, mpfb.npz and groups.npz (the template), ref.npz (the dense bald model, placed by place_ref.py).

Without landmarks an ear of the template does not reach the ear of the model: its vertices find the nearest surface,
which is the inner side of the model's ear, and the rim stays short (the second face: 11 mm a side in the front view).
An ear is found the same way on both meshes, from the side: it is what stands out of the side of the head. The depth
map of that side (how far the surface is from the middle plane) is opened with a disc wider than an ear, which leaves
the head without it; the ear is where the map stands over the opened one, and its rim is where it also drops by
5 mm within 3 mm. The rim is then walked by direction, from
the front of the top round the back to the front of the lobe, in the frame of the ear's own box: for a direction, the
rim point is the one furthest that way. The template's rim vertex for a direction takes the model's rim point for the
same direction; one more pair is the point of each ear furthest from the head. (Moving the pairs from the edge to the
crest of the helix lost the edge: the lobes came out 10 mm short.) The front of the ear has no rim (it grows out of the cheek)
and gets no landmark.
Names are rim_l_NN and rim_l_out (+x, the subject's left), rim_r_NN and rim_r_out. Landmarks of the input whose names
start with "ear", "tragus" or "rim_" are replaced. The landmarks hold to the end of the wrap: let go in its last
steps, the lobes fall back by 9 mm a side.
--mask writes which vertices of the template are an ear (ear: a flag per vertex): the skin of everything that stands
out there, to 6 mm under its outer side. The hairstyle keeps off it (hair/groom.py): by its own rule, "skin further
than 84 mm from the middle", a wide head has no ears and hair lies on them.
--keep-hand leaves the landmarks as they are (ears picked by hand, lm_pick.py) and only writes the mask.
"""
import sys, os, json
import numpy as np
from scipy import ndimage as nd

KEEP_HAND = '--keep-hand' in sys.argv
if KEEP_HAND: sys.argv.remove('--keep-hand')
MASK = None
if '--mask' in sys.argv:
    _i = sys.argv.index('--mask'); MASK = sys.argv[_i+1]; del sys.argv[_i:_i+2]
D, SRC, OUT = sys.argv[1:4]; CHECK = sys.argv[4] if len(sys.argv) > 4 else None
V = json.load(open(os.path.join(D, 'views.json'))); eye = np.array(V['eye'])
m = np.load(os.path.join(D, 'mpfb.npz')); body = np.load(os.path.join(D, 'groups.npz'))['body'].astype(bool)
co = m['co'].astype(np.float64); tris = m['tris'][body[m['tris']].all(1)]
ref = np.load(os.path.join(D, 'ref.npz'))['v'].astype(np.float64)

CELL = 0.001                                         # the side map: 1 mm cells
Y0, Y1, Z0, Z1 = 0.02, 0.16, -0.085, 0.06            # where an ear can be: behind the eyes, round their height
DISC = 0.023                                         # radius of the opening: an ear is under 4 cm wide
OVER = 0.003                                         # an ear stands this far over the opened side of the head
CLIFF = 0.005                                        # and its rim this far over the head next to it
ANGLES = np.radians(np.arange(-40, 221, 20))         # 0 = up, 90 = back, 180 = down; the front has no rim
ny, nz = int(round((Y1-Y0)/CELL)), int(round((Z1-Z0)/CELL))
yy, zz = np.mgrid[-23:24, -23:24]; FOOT = np.hypot(yy, zz) <= DISC/CELL


def samples(p, t, step=0.0006):
    """Points over the triangles, dense enough for the side map."""
    a, b, c = p[t[:, 0]], p[t[:, 1]], p[t[:, 2]]; n = np.ceil(np.maximum(np.linalg.norm(b-a, axis=1), np.linalg.norm(c-a, axis=1))/step).astype(int).clip(1, 40)
    out = []
    for k in np.unique(n):
        s = n == k; u, v = np.mgrid[0:k+1, 0:k+1]; ok = u+v <= k; u, v = u[ok]/k, v[ok]/k
        out.append((a[s][:, None]+(b[s]-a[s])[:, None]*u[None, :, None]+(c[s]-a[s])[:, None]*v[None, :, None]).reshape(-1, 3))
    return np.concatenate(out)


def ear(pts, side):
    """The side map of the head and the ear in it: (map of side*x in metres, the rim's cells, their box centre and half
    sizes in cells, all cells of the ear)."""
    q = pts-eye; s = (side*q[:, 0] > 0.03) & (q[:, 1] > Y0) & (q[:, 1] < Y1) & (q[:, 2] > Z0) & (q[:, 2] < Z1); q = q[s]
    i, j = ((q[:, 1]-Y0)/CELL).astype(int), ((q[:, 2]-Z0)/CELL).astype(int)
    M = np.full((ny, nz), -1.0); np.maximum.at(M, (i, j), side*q[:, 0]); hole = M < 0
    if hole.any():
        k = nd.distance_transform_edt(hole, return_distances=False, return_indices=True); M = M[k[0], k[1]]
    M = nd.median_filter(M, 3); over = M-nd.grey_opening(M, footprint=FOOT)
    # the rim: cells of the ear that stand CLIFF over a neighbour within 3 mm. The side of a skull bulges too, and the
    # opening takes 4-5 mm off it, but only an ear has an edge
    mask = (over > OVER) & (M-nd.minimum_filter(M, 7) > CLIFF) & ~nd.binary_dilation(hole, iterations=3)
    lab, n = nd.label(mask, np.ones((3, 3)))                         # specks next to the ear are not its rim
    if n: mask &= np.isin(lab, 1+np.nonzero(nd.sum(mask, lab, range(1, n+1)) >= 40)[0])
    lab, n = nd.label(nd.binary_dilation(mask, iterations=2))
    if not n: return M, None, None, None, None
    mask &= lab == 1+np.argmax(nd.sum(mask, lab, range(1, n+1)))
    ii, jj = np.nonzero(mask); c = np.array([(ii.min()+ii.max())/2, (jj.min()+jj.max())/2]); h = np.array([(ii.max()-ii.min())/2, (jj.max()-jj.min())/2])
    lab, n = nd.label(nd.binary_opening(over > OVER))                # the whole ear: what stands out and holds the rim
    whole = nd.binary_fill_holes(np.isin(lab, np.unique(lab[mask & (lab > 0)])) | mask)
    return M, mask, c, h, whole


def support(ii, jj, c, h, a):
    """Index of the point furthest in direction a (0 = up, 90 = back) in the frame of the ear's box."""
    return int(np.argmax(np.sin(a)*(ii-c[0])/h[0]+np.cos(a)*(jj-c[1])/h[1]))


dense = samples(co, tris); out = []; drawn = []; ok = True; earv = np.zeros(len(co), bool)
for side, tag in ((1, 'l'), (-1, 'r')):
    Mr, kr, cr, hr, _ = ear(ref, side); Mt, kt, ct, ht, wt = ear(dense, side)
    q = co-eye; i, j = ((q[:, 1]-Y0)/CELL).astype(int), ((q[:, 2]-Z0)/CELL).astype(int)
    inside = body & (side*q[:, 0] > 0.03) & (i >= 0) & (i < ny) & (j >= 0) & (j < nz)
    if kt is not None:
        v_ = np.nonzero(inside)[0]; v_ = v_[nd.binary_dilation(wt, iterations=2)[i[v_], j[v_]]]; earv[v_[side*q[v_, 0] > Mt[i[v_], j[v_]]-0.006]] = True
    if kr is None or kt is None: print(f'EARS {tag}: no ear found on the', 'model' if kr is None else 'template'); ok = False; continue
    size = lambda h: (2*h*CELL*1000).round(0)
    print(f'EARS {tag}: model {size(hr)[0]:.0f} x {size(hr)[1]:.0f} mm, {kr.sum()} cells; template {size(ht)[0]:.0f} x {size(ht)[1]:.0f} mm, {kt.sum()} cells')
    if not (18 <= size(hr)[0] <= 50 and 40 <= size(hr)[1] <= 85): print(f'EARS {tag}: the model\'s ear has an odd size, no landmarks'); ok = False; continue
    # the template's rim vertices: skin on the ear's cells, in the outer layer of the side map
    kt2 = nd.binary_dilation(kt, iterations=2); cand = np.nonzero(inside)[0]; cand = cand[kt2[i[cand], j[cand]]]
    cand = cand[side*q[cand, 0] > Mt[i[cand], j[cand]]-0.003]
    ri, rj = np.nonzero(kr); used = set(); pairs = []
    picks = [('%02d' % n, cand[support(i[cand], j[cand], ct, ht, a)], support(ri, rj, cr, hr, a)) for n, a in enumerate(ANGLES)]
    picks.append(('out', cand[np.argmax(side*q[cand, 0])], int(np.argmax(Mr[ri, rj]))))   # and the point furthest from the head
    for n, v, k in picks:
        if v in used: continue
        used.add(v); tgt = eye+np.array([side*Mr[ri[k], rj[k]], Y0+(ri[k]+0.5)*CELL, Z0+(rj[k]+0.5)*CELL])
        out.append({'name': 'rim_%s_%s' % (tag, n), 'mpfb_vertex': int(v), 'target': [round(float(x), 5) for x in tgt]}); pairs.append((v, tgt))
    mv = np.array([t-co[v] for v, t in pairs])*1000
    print(f'EARS {tag}: {len(pairs)} rim landmarks; move mm: mean x {mv[:, 0].mean():+.1f} y {mv[:, 1].mean():+.1f} z {mv[:, 2].mean():+.1f}, longest {np.linalg.norm(mv, axis=1).max():.1f}')
    drawn.append((Mr, kr, Mt, kt, pairs))

base = json.load(open(SRC)); keep = [l for l in base['landmarks'] if not l['name'].startswith(('ear', 'tragus', 'rim_'))]
if KEEP_HAND: json.dump(base, open(OUT, 'w'), indent=1)
else: json.dump({'note': base.get('note', '')+('; ears by ear_points.py' if out else '; no ears found'), 'landmarks': keep+out}, open(OUT, 'w'), indent=1)
print('EARS total', 0 if KEEP_HAND else len(out), 'landmarks' if ok else 'landmarks (one or both ears failed)', '(hand picks kept)' if KEEP_HAND else '')
if MASK:
    np.savez(MASK, ear=earv); print('EARS mask', int(earv.sum()), 'vertices of the template')

if CHECK and drawn:
    from PIL import Image
    K = 4; rows = []
    for Mr, kr, Mt, kt, pairs in drawn:
        tiles = []
        for M, k, pts in ((Mr, kr, [t for _, t in pairs]), (Mt, kt, [co[v] for v, _ in pairs])):
            g = np.clip((M-M.min())/(M.max()-M.min()+1e-9), 0, 1); img = np.stack([g*255]*3, -1)
            img[k] = img[k]*0.5+np.array([255, 200, 0])*0.5
            img = np.kron(img[:, ::-1].transpose(1, 0, 2), np.ones((K, K, 1)))             # up is up, the face to the left
            for p in pts:
                a, b = int(((p-eye)[1]-Y0)/CELL*K), int((nz-((p-eye)[2]-Z0)/CELL)*K)
                img[max(b-3, 0):b+4, max(a-3, 0):a+4] = (255, 40, 40)
            tiles.append(img)
        rows.append(np.hstack(tiles))
    Image.fromarray(np.vstack(rows).astype(np.uint8)).save(CHECK, quality=88)
