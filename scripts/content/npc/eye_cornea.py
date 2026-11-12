# The cornea over the iris of a head (reports/content/2026-11-hero-head/eye-shader.md, "Способ" 5, task 2): MPFB's iris
# is a flat cone sunk ~2.5 mm under the ball; its vertices go up onto the sphere of the white along the eye's axis (the
# iris keeps its place seen from the front) and every vertex of the front gets the normal of a corneal dome: a sphere of
# 0.646 of the ball's radius (7.75 mm of a 12 mm eye) that meets the white at the limbus, blended into it by a smooth
# maximum, no step. The dome is in the normals and in the depth of the iris under it (eye_material.py --depth), not in
# the geometry: our lids lie 0.0-0.8 mm over the white above the iris (blink_build fitted them to the sphere), a dome of
# the real proportions stands 0.4-1.0 mm over it there and would come through them.
#   python eye_cornea.py <head.glb> <eye_material.json> <out.glb>
# eye_material.json is eye_material.py's: where each iris lies in the eye map (pupil, limbus). Used by
# export_test_head.py (--cornea) on the GLB it writes.
import sys, json, struct
import numpy as np

ROUND_CORNEA = 7.75/12.0        # the cornea's radius of curvature of the ball's radius (7.7-7.8 mm of a 12 mm eye)
BLEND_CM = 0.025                # the smooth maximum of the white and the dome: they blend over about +-0.6 mm
DEPTH_MAX_CM = 0.5              # the depth of the iris under the cornea in the ORM's b: b = depth / this (main.frag)


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

    def add(self, arr):
        """A float VEC3 attribute as a new accessor of its own buffer view; its index."""
        arr = np.ascontiguousarray(arr, np.float32)
        self.bin += b'\0'*((4-len(self.bin) % 4) % 4)
        self.j['bufferViews'].append({'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': arr.nbytes, 'target': 34962})
        self.bin += arr.tobytes()
        self.j['accessors'].append({'bufferView': len(self.j['bufferViews'])-1, 'componentType': 5126,
                                    'count': int(arr.shape[0]), 'type': 'VEC3',
                                    'min': arr.min(0).tolist(), 'max': arr.max(0).tolist()})
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


# ---- the head ---------------------------------------------------------------------------------------------------
def part(glb, name):
    p = glb.prim(name); at = p['attributes']
    return (to_engine(glb.acc(at['POSITION'])), glb.acc(at['NORMAL']).astype(np.float64)*np.sign(FLIP),
            glb.acc(at['TEXCOORD_0']).astype(np.float64), glb.acc(p['indices']).reshape(-1, 3).astype(np.int64))


def apply(src, eye_json, dst, log=print):
    """The head GLB src with the iris on the white and the dome's normals, written to dst."""
    glb = Glb(src)
    for side, (pupil, limbus) in zip(('eye_L', 'eye_R'), irises(eye_json)):
        P, N, UV, T = part(glb, side); cor = Cornea(P, UV, pupil, limbus)
        log('CORNEA %s: white r %.3f cm, limbus %.3f cm off the axis (%.3f UV), dome r %.3f cm, depth at the centre %.2f mm'
            % (side, cor.r, cor.limbus, limbus, cor.rc, 10*float(cor.depth(np.array([0.0]))[0])))
        P2, N2, raised, normals = cor.apply(P, N, UV, pupil, limbus)
        pr = glb.prim(side)['attributes']
        pr['POSITION'] = glb.add(to_gltf(P2)); pr['NORMAL'] = glb.add(N2*np.sign(FLIP))
        log('  %d vertices of the iris onto the white, %d normals of the dome' % (raised, normals))
    glb.save(dst)


if __name__ == '__main__':
    apply(*sys.argv[1:4])
