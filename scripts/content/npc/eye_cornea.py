# The cornea over the iris and the wet seam along the lower lid of a head (reports/content/2026-11-hero-head/eye-shader.md,
# "Способ" 5-6, task 2): the head GLB of export_test_head.py gets
# - the cornea: MPFB's iris is a flat cone sunk ~2.5 mm under the ball; its vertices go up onto the sphere of the white
#   along the eye's axis (the iris keeps its place seen from the front) and every vertex of the front gets the normal of
#   a corneal dome: a sphere of 0.646 of the ball's radius (7.75 mm of a 12 mm eye) that meets the white at the limbus,
#   blended into it by a smooth maximum, no step. The dome is in the normals and in the depth of the iris under it
#   (eye_material.py --depth), not in the geometry: our lids lie 0.0-0.8 mm over the white above the iris (blink_build
#   fitted them to the sphere), a dome of the real proportions stands 0.4-1.0 mm over it there and would come through them;
# - the tear line: a thin ribbon from the edge of the lower lid up onto the ball, each of its vertices bound to the lid's
#   edge (the face shapes move it with the lid: blinks, squints, speech), MASK, a material of its own (OWN_<head>_TEARS).
# The lid's edge is found as the face rig finds it (LidShade, common/graphics/mesh/lidshade.cpp): the lids' triangles
# seen from the ball's centre, the open gap between them in each column of yaw.
#   python eye_cornea.py <head.glb> <eye_material.json> <out.glb> [--no-cornea] [--no-tears] [--maps <dir>]
# eye_material.json is eye_material.py's: where each iris lies in the eye map (pupil, limbus). --maps writes the tear
# line's maps (tears.png, tears_orm.png) there. Used by export_test_head.py (--cornea, --tear-line) on the GLB it writes.
import sys, os, json, struct, zlib
import numpy as np

ROUND_CORNEA = 7.75/12.0        # the cornea's radius of curvature of the ball's radius (7.7-7.8 mm of a 12 mm eye)
BLEND_CM = 0.025                # the smooth maximum of the white and the dome: they blend over about +-0.6 mm
DEPTH_MAX_CM = 0.5              # the depth of the iris under the cornea in the ORM's b: b = depth / this (main.frag)
TEAR_WIDTH_CM = 0.08            # the tear line from the lid's edge up the ball, cm (0.8 mm on a ball ~1.5 times a real one)
TEAR_LIFT_CM = (0.012, 0.008)   # its middle and upper row over the white (the ball turns under it)
TEAR_ROUGH = 0.05
TEAR_ALPHA = 0.85               # the ribbon's coverage at its strongest (hashed alpha); fades to the ends and the top
# Its colour: warm, light, near the lid's wet rim. The line reads by it more than by its highlight (it is not an eye: no
# lids' shade, the sun lights it in full, 3-5 times the white next to it). The colour of the white in the lids' shade
# (120, 100, 95 at coverage 0.6) left it as dark as the eye there was without it: no line (eye-shader.md, task 2)
TEAR_SRGB = (205, 178, 168)
# LidShade's constants (lidshade.h): yaw columns, how far up and down, the band of the lids round the ball
COL_FROM, COL_STEP, COLUMNS, REACH, BAND = -70.0, 2.0, 71, 80.0, 0.25


# ---- GLB: read, add arrays, write -------------------------------------------------------------------------------
class Glb:
    TYPES = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}
    COMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}

    def __init__(self, path):
        d = open(path, 'rb').read()
        jl = struct.unpack_from('<I', d, 12)[0]
        self.j = json.loads(d[20:20+jl])
        bl = struct.unpack_from('<I', d, 20+jl)[0]
        self.bin = bytearray(d[28+jl:28+jl+bl])

    def _view(self, bv, off, count, ctype, n):
        v = self.j['bufferViews'][bv]; dt = self.TYPES[ctype]; sz = np.dtype(dt).itemsize
        o = v.get('byteOffset', 0)+off; st = v.get('byteStride', sz*n)
        raw = np.frombuffer(bytes(self.bin[o:o+st*count]), np.uint8).reshape(count, st)[:, :sz*n].copy()
        return raw.view(dt).reshape(count, n)

    def acc(self, i):
        a = self.j['accessors'][i]; n = self.COMP[a['type']]
        if 'bufferView' in a:
            out = self._view(a['bufferView'], a.get('byteOffset', 0), a['count'], a['componentType'], n)
        else:
            out = np.zeros((a['count'], n), self.TYPES[a['componentType']])
        if 'sparse' in a:                                     # Blender writes the morph targets sparse
            s = a['sparse']; out = out.copy()
            idx = self._view(s['indices']['bufferView'], s['indices'].get('byteOffset', 0), s['count'],
                             s['indices']['componentType'], 1)[:, 0].astype(np.int64)
            out[idx] = self._view(s['values']['bufferView'], s['values'].get('byteOffset', 0), s['count'],
                                  a['componentType'], n)
        return out

    def add(self, arr, indices=False):
        arr = np.ascontiguousarray(arr, np.uint32 if indices else np.float32)
        self.bin += b'\0'*((4-len(self.bin) % 4) % 4)
        self.j['bufferViews'].append({'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': arr.nbytes,
                                      'target': 34963 if indices else 34962})
        self.bin += arr.tobytes()
        a = {'bufferView': len(self.j['bufferViews'])-1, 'componentType': 5125 if indices else 5126,
             'count': int(len(arr)) if indices else int(arr.shape[0]),
             'type': 'SCALAR' if indices else {2: 'VEC2', 3: 'VEC3'}[arr.shape[1]]}
        if not indices and arr.shape[1] == 3:
            a['min'] = arr.min(0).tolist(); a['max'] = arr.max(0).tolist()
        self.j['accessors'].append(a)
        return len(self.j['accessors'])-1

    def node(self, name):
        return next((n for n in self.j['nodes'] if n.get('name') == name), None)

    def prim(self, name):
        return self.j['meshes'][self.node(name)['mesh']]['primitives'][0]

    def save(self, path):
        self.bin += b'\0'*((4-len(self.bin) % 4) % 4)
        self.j['buffers'] = [{'byteLength': len(self.bin)}]
        txt = json.dumps(self.j, separators=(',', ':')).encode(); txt += b' '*((4-len(txt) % 4) % 4)
        out = struct.pack('<III', 0x46546C67, 2, 28+len(txt)+len(self.bin)) + struct.pack('<II', len(txt), 0x4E4F534A) + \
            txt + struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin)
        open(path, 'wb').write(out)


# glTF (metres: x = -up, y = across, z = forward) <-> the engine's frame of the head bone (cm: x up, y across, z forward)
FLIP = np.array([-100.0, 100.0, 100.0])
to_engine = lambda p: np.asarray(p, np.float64)*FLIP
to_gltf = lambda p: np.asarray(p, np.float64)/FLIP


def fit_sphere(p):
    A = np.c_[2*p, np.ones(len(p))]; x = np.linalg.lstsq(A, (p**2).sum(1), rcond=None)[0]
    return x[:3], float(np.sqrt(x[3]+x[:3] @ x[:3]))


def smax(a, b, k):
    """A smooth maximum (polynomial): max(a, b) away from a = b, the two blended over |a-b| < k, its slope continuous."""
    q = np.maximum(k-np.abs(a-b), 0)/k
    return np.maximum(a, b)+0.25*k*q*q


# ---- the ball and its cornea ------------------------------------------------------------------------------------
class Cornea:
    """One ball (engine frame, cm) and the dome over its iris. pupil_uv and limbus_uv: the iris' centre in the eye map and
    the radius in UV of its dark ring's middle (eye_material.py)."""

    def __init__(self, P, UV, pupil_uv, limbus_uv):
        du = np.linalg.norm(UV-np.asarray(pupil_uv), axis=1)
        white = du > 1.3*limbus_uv
        assert white.sum() > 20 and (du < 0.5*limbus_uv).sum() > 3, 'the iris is not on this ball'
        self.c, self.r = fit_sphere(P[white])                 # the sphere of the white, without the sunk iris
        a = P[du < 0.5*limbus_uv].mean(0)-self.c; self.axis = a/np.linalg.norm(a)
        rho = self.rho(P)
        ring = (du > 0.6*limbus_uv) & (du < 1.6*limbus_uv) & (((P-self.c) @ self.axis) > 0)
        k = np.polyfit(du[ring], rho[ring], 1)                # the limbus in the ball: UV radius -> cm off the axis
        self.limbus = float(np.polyval(k, limbus_uv))
        self.rc = ROUND_CORNEA*self.r
        assert self.limbus < 0.8*self.rc, 'the limbus is too wide for the dome'
        self.hc = self.white(self.limbus)-np.sqrt(self.rc**2-self.limbus**2)

    def rho(self, P):
        d = P-self.c
        return np.linalg.norm(d-np.outer(d @ self.axis, self.axis), axis=1)

    def white(self, rho):
        return np.sqrt(np.maximum(self.r**2-rho**2, 0))

    def dome(self, rho):
        return self.hc+np.sqrt(np.maximum(self.rc**2-np.minimum(rho, self.rc)**2, 0))

    def height(self, rho):
        """The cornea's surface along the axis from the centre: the white and the dome, blended."""
        return smax(self.white(rho), self.dome(rho), BLEND_CM)

    def depth(self, rho):
        """How deep the iris lies under the dome, cm: from the dome down to the plane of the limbus, 0 past it, smooth."""
        return smax(self.dome(rho)-self.dome(self.limbus), 0.0*rho, BLEND_CM)

    def normals(self, P):
        """The dome's normals at the points (their rho), outward."""
        d = P-self.c; h = d @ self.axis; e = d-np.outer(h, self.axis); rho = np.linalg.norm(e, axis=1)
        e = e/np.maximum(rho, 1e-9)[:, None]
        eps = 1e-4; slope = (self.height(rho+eps)-self.height(np.maximum(rho-eps, 0)))/(rho+eps-np.maximum(rho-eps, 0))
        n = self.axis[None, :]-slope[:, None]*e
        return n/np.linalg.norm(n, axis=1)[:, None]

    def apply(self, P, N, UV, pupil_uv, limbus_uv):
        """The ball with the iris on the white and the dome's normals over the front (engine frame)."""
        du = np.linalg.norm(UV-np.asarray(pupil_uv), axis=1)
        d = P-self.c; h = d @ self.axis; rho = self.rho(P)
        sunk = (du < 1.0*limbus_uv) & (np.linalg.norm(d, axis=1) < self.r-0.005) & (h > 0)
        e = d-np.outer(h, self.axis); e = e/np.maximum(np.linalg.norm(e, axis=1), 1e-9)[:, None]
        P2 = P.copy()
        P2[sunk] = self.c+e[sunk]*rho[sunk, None]+np.outer(self.white(rho[sunk]), self.axis)
        N2 = N.copy(); front = (du < 1.3*limbus_uv) & (h > 0)
        N2[front] = self.normals(P2[front])
        return P2, N2, sunk.sum(), front.sum()


def irises(eye_json):
    """eye_material.json -> per ball (eye_L, eye_R) the pupil and the limbus in UV."""
    info = json.load(open(eye_json))
    return [(np.array(i['pupil_uv']), float(i['limbus_uv'] if 'limbus_uv' in i else limbus_uv(i['iris_radius_uv'])))
            for i in info['irises']]


def limbus_uv(iris_radius_uv):
    """The middle of the dark ring eye_material.py paints: 0.15 of its width (12 % of the diameter) past the iris' edge."""
    return iris_radius_uv*(1+0.15*0.12*2)


# ---- the lid's edge, as LidShade finds it ------------------------------------------------------------------------
def lower_edge(F, tris, c, r):
    """Per column of yaw (LidShade's): the lower lid's edge as seen from the ball's centre c, None where the lids meet.
    Each: (yaw, tan of pitch, (vertex i, vertex j, t)) - the point on the lid's triangle edge i-j that makes it."""
    d = F-c; l = np.linalg.norm(d, axis=1)
    near = (np.abs(l-r) <= BAND*r) & (d[:, 2] >= np.cos(np.radians(REACH))*l)
    tri = tris[near[tris].all(1)]
    reach = np.tan(np.radians(REACH))
    h = np.hypot(d[:, 1], d[:, 2]); z = np.maximum(d[:, 2], 1e-3*h+1e-6)
    across = d[:, 1]/z; rise = np.clip(d[:, 0]/np.maximum(h, 1e-6), -reach, reach)
    cols = np.tan(np.radians(COL_FROM+COL_STEP*np.arange(COLUMNS)))
    spans = [[] for _ in range(COLUMNS)]
    for t in tri:
        y = across[t]; p = rise[t]
        for ci in np.nonzero((cols >= y.min()) & (cols <= y.max()))[0]:
            cy = cols[ci]; lo = (reach, None); hi = (-reach, None)
            for k in range(3):
                n = (k+1) % 3
                if (y[k]-cy)*(y[n]-cy) > 0 or y[k] == y[n]:
                    continue
                f = (cy-y[k])/(y[n]-y[k]); q = p[k]+(p[n]-p[k])*f
                if q < lo[0]:
                    lo = (q, (t[k], t[n], f))
                if q > hi[0]:
                    hi = (q, (t[k], t[n], f))
            if lo[0] <= hi[0]:
                spans[ci].append((lo[0], hi[0], hi[1]))

    def gaps(s):
        s = sorted(s, key=lambda x: x[0]); end = (-reach, None); out = []
        for a, b, eb in s:
            if a > end[0]:
                out.append((end, min(a, reach)))
            if b > end[0]:
                end = (b, eb)
            if end[0] >= reach:
                break
        if end[0] < reach:
            out.append((end, reach))
        return out
    mid = int(round(-COL_FROM/COL_STEP)); near30 = np.tan(np.radians(30))
    best = None
    for (g0, g1) in gaps(spans[mid]):
        if g1 <= g0[0]:
            continue
        dist = 0 if g0[0] <= 0 <= g1 else min(abs(g0[0]), abs(g1))
        if dist <= near30 and (best is None or dist < best[0]):
            best = (dist, g0, g1)
    out = [None]*COLUMNS
    if best is None:
        return out
    out[mid] = (best[1], best[2])
    for step in (1, -1):
        prev = out[mid]
        for ci in range(mid+step, COLUMNS if step > 0 else -1, step):
            if prev is None or prev[0][0] >= prev[1]:
                break
            at = 0.5*(prev[0][0]+prev[1]); lo = (-reach, None); hi = reach; shut = False
            for a, b, eb in spans[ci]:
                if a < at < b:
                    shut = True
                if b <= at and b > lo[0]:
                    lo = (b, eb)
                if a >= at:
                    hi = min(hi, a)
            if shut or lo[1] is None:
                break
            out[ci] = prev = (lo, hi)
    return [(COL_FROM+COL_STEP*ci, o[0][0], o[0][1]) if o is not None else None for ci, o in enumerate(out)]


def tear_line(F, tris, deltas, c, r):
    """The ribbon along the lower lid of one ball: positions, normals, UV, triangles (engine frame) and per face shape the
    deltas of its vertices (those of the lid's edge it is bound to)."""
    edge = [e for e in lower_edge(F, tris, c, r) if e is not None]
    assert len(edge) >= 8, 'no lower lid round the ball'
    rows = []                                                      # per column: bottom (on the lid), middle, top (ball)
    bind = []
    for yaw, tp, (i, j, f) in edge:
        lid = F[i]*(1-f)+F[j]*f
        pitch = np.arctan(tp); y = np.radians(yaw)
        on = lambda p, lift: c+(r+lift)*np.array([np.sin(p), np.cos(p)*np.sin(y), np.cos(p)*np.cos(y)])
        rows.append([lid, on(pitch+0.25*TEAR_WIDTH_CM/r, TEAR_LIFT_CM[0]), on(pitch+TEAR_WIDTH_CM/r, TEAR_LIFT_CM[1])])
        bind.append((i, j, f))
    V = np.array(rows)                                             # columns x 3 x 3
    nc = len(V)
    # normals: the ball's at the top, up the meniscus (out of the ribbon, away from the lid) at the bottom
    radial = (V-c)/np.linalg.norm(V-c, axis=2, keepdims=True)
    along = np.gradient(V, axis=0); across = V[:, 2:3]-V[:, 0:1]
    face = np.cross(along, np.repeat(across, 3, 1)); face /= np.linalg.norm(face, axis=2, keepdims=True)
    face *= np.sign((face*radial).sum(2, keepdims=True))
    Nn = np.stack([face[:, 0], 0.5*(face[:, 1]+radial[:, 1]), radial[:, 2]], 1)
    Nn /= np.linalg.norm(Nn, axis=2, keepdims=True)
    u = np.linspace(0, 1, nc); UVs = np.stack([np.repeat(u[:, None], 3, 1), np.tile([0.0, 0.25, 1.0], (nc, 1))], 2)
    idx = np.arange(nc*3).reshape(nc, 3); T = []
    for k in range(nc-1):
        for m in range(2):
            a, b, cc, dd = idx[k, m], idx[k+1, m], idx[k+1, m+1], idx[k, m+1]
            T += [(a, b, cc), (a, cc, dd)]
    T = np.array(T)
    # the winding of the head's own parts: counter-clockwise from outside in glTF, so clockwise in this mirrored frame
    # (the engine culls the back)
    fn = np.cross(V.reshape(-1, 3)[T[:, 1]]-V.reshape(-1, 3)[T[:, 0]], V.reshape(-1, 3)[T[:, 2]]-V.reshape(-1, 3)[T[:, 0]])
    if ((fn*(V.reshape(-1, 3)[T[:, 0]]-c)).sum(1) > 0).mean() > 0.5:
        T = T[:, [0, 2, 1]]
    D = {}
    for name, dl in deltas.items():
        dv = np.array([dl[i]*(1-f)+dl[j]*f for i, j, f in bind])
        if np.abs(dv).max() > 1e-6:
            D[name] = np.repeat(dv[:, None], 3, 1).reshape(-1, 3)
    return V.reshape(-1, 3), Nn.reshape(-1, 3), UVs.reshape(-1, 2), T, D


def tear_maps():
    """The tear line's colour (RGBA) and ORM maps: coverage fades to the corners and up the ball."""
    W, H = 256, 32
    u = (np.arange(W)+0.5)/W; v = (np.arange(H)+0.5)/H
    st = lambda x, a, b: np.clip((x-a)/(b-a), 0, 1)**2*(3-2*np.clip((x-a)/(b-a), 0, 1))
    alpha = TEAR_ALPHA*np.outer(1-st(v, 0.45, 1.0), st(u, 0.0, 0.12)*(1-st(u, 0.88, 1.0)))
    rgba = np.zeros((H, W, 4)); rgba[..., :3] = np.array(TEAR_SRGB)/255; rgba[..., 3] = alpha
    orm = np.zeros((H, W, 3)); orm[..., 0] = 1; orm[..., 1] = TEAR_ROUGH
    return rgba, orm


def write_png(path, img):
    """A PNG of a float image 0..1 (RGB or RGBA), without PIL (Blender's Python has none)."""
    a = (np.clip(img, 0, 1)*255+0.5).astype(np.uint8); h, w, ch = a.shape
    raw = b''.join(b'\0'+a[y].tobytes() for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d))+t+d+struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6 if ch == 4 else 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9))+chunk(b'IEND', b''))


# ---- the head ---------------------------------------------------------------------------------------------------
def part(glb, name):
    p = glb.prim(name); at = p['attributes']
    return (to_engine(glb.acc(at['POSITION'])), glb.acc(at['NORMAL']).astype(np.float64)*np.sign(FLIP),
            glb.acc(at['TEXCOORD_0']).astype(np.float64), glb.acc(p['indices']).reshape(-1, 3).astype(np.int64))


def apply(src, eye_json, dst, cornea=True, tears=True, log=print):
    """The head GLB src with the cornea and the tear line, written to dst. Returns the tear line's material or None."""
    glb = Glb(src); found = irises(eye_json)
    F, FN, FUV, FT = part(glb, 'face')
    mesh = glb.j['meshes'][glb.node('face')['mesh']]
    names = mesh.get('extras', {}).get('targetNames', [])
    deltas = {n: to_engine(glb.acc(t['POSITION'])) for n, t in zip(names, glb.prim('face').get('targets', []))}
    strips = []
    for side, (pupil, limbus) in zip(('eye_L', 'eye_R'), found):
        P, N, UV, T = part(glb, side); cor = Cornea(P, UV, pupil, limbus)
        log('CORNEA %s: white r %.3f cm, limbus %.3f cm off the axis (%.3f UV), dome r %.3f cm, depth at the centre %.2f mm'
            % (side, cor.r, cor.limbus, limbus, cor.rc, 10*float(cor.depth(np.array([0.0]))[0])))
        if cornea:
            P2, N2, raised, normals = cor.apply(P, N, UV, pupil, limbus)
            pr = glb.prim(side)['attributes']
            pr['POSITION'] = glb.add(to_gltf(P2)); pr['NORMAL'] = glb.add(N2*np.sign(FLIP))
            log('  %d vertices of the iris onto the white, %d normals of the dome' % (raised, normals))
        if tears:
            strips.append(tear_line(F, FT, deltas, cor.c, cor.r))
    if not tears:
        glb.save(dst)
        return None
    V = np.concatenate([s[0] for s in strips]); Nn = np.concatenate([s[1] for s in strips])
    UVs = np.concatenate([s[2] for s in strips]); T = np.concatenate([strips[0][3], strips[1][3]+len(strips[0][0])])
    keys = [n for n in names if n in strips[0][4] or n in strips[1][4]]
    targets = []
    for n in keys:
        D = np.concatenate([s[4].get(n, np.zeros((len(s[0]), 3))) for s in strips])
        targets.append({'POSITION': glb.add(to_gltf(D)), 'NORMAL': glb.add(np.zeros_like(D))})
    eye_mat = glb.j['materials'][glb.prim('eye_L')['material']]['name']
    assert eye_mat.endswith('_EYE'), eye_mat
    mat = eye_mat[:-len('EYE')]+'TEARS'
    glb.j['materials'].append({'name': mat, 'doubleSided': True, 'alphaMode': 'MASK'})
    prim = {'attributes': {'POSITION': glb.add(to_gltf(V)), 'NORMAL': glb.add(Nn*np.sign(FLIP)), 'TEXCOORD_0': glb.add(UVs)},
            'indices': glb.add(T.ravel(), indices=True), 'material': len(glb.j['materials'])-1}
    if targets:
        prim['targets'] = targets
    glb.j['meshes'].append({'name': 'tears', 'primitives': [prim], **({'extras': {'targetNames': keys}} if keys else {})})
    glb.j['nodes'].append({'name': 'tears', 'mesh': len(glb.j['meshes'])-1})
    glb.j['scenes'][glb.j.get('scene', 0)]['nodes'].append(len(glb.j['nodes'])-1)
    log('TEARS %d vertices, %d triangles, %d face shapes move them' % (len(V), len(T), len(keys)))
    glb.save(dst)
    return mat


if __name__ == '__main__':
    a = sys.argv[1:]
    maps = a[a.index('--maps')+1] if '--maps' in a else None
    if maps:
        os.makedirs(maps, exist_ok=True); rgba, orm = tear_maps()
        write_png(os.path.join(maps, 'tears.png'), rgba); write_png(os.path.join(maps, 'tears_orm.png'), orm)
    pos = [x for i, x in enumerate(a) if not x.startswith('--') and (i == 0 or a[i-1] != '--maps')]
    apply(pos[0], pos[1], pos[2], cornea='--no-cornea' not in a, tears='--no-tears' not in a)
