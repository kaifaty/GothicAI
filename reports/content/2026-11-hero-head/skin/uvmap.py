# Texels of a UV map and the surface under them.
#   pos, nor, hit = uvmap.raster(co, no, tri, tuv, N, faces)
# For every texel of an N x N map covered by the given triangles (indices into tri/tuv): the point and the normal
# of the surface there, and hit = the texel is covered. A small overdraw closes the gaps between neighbours.
import numpy as np


def raster(co, no, tri, tuv, N, faces):
    pos = np.zeros((N, N, 3), np.float32); nor = np.zeros((N, N, 3), np.float32); hit = np.zeros((N, N), bool)
    for k in faces:
        a = tuv[k]*[N, N]; a[:, 1] = N-a[:, 1]
        x0, y0 = np.floor(a.min(0)).astype(int)-1; x1, y1 = np.ceil(a.max(0)).astype(int)+1
        x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, N-1), min(y1, N-1)
        X, Y = np.meshgrid(np.arange(x0, x1+1)+0.5, np.arange(y0, y1+1)+0.5)
        T = np.array([[a[0, 0]-a[2, 0], a[1, 0]-a[2, 0]], [a[0, 1]-a[2, 1], a[1, 1]-a[2, 1]]])
        if abs(np.linalg.det(T)) < 1e-9: continue
        l = np.linalg.solve(T, np.stack([X.ravel()-a[2, 0], Y.ravel()-a[2, 1]])); w = np.vstack([l, 1-l.sum(0)]).T
        ok = (w > -0.02).all(1); yy, xx = (Y.ravel()[ok]).astype(int), (X.ravel()[ok]).astype(int)
        pos[yy, xx] = w[ok] @ co[tri[k]]; nor[yy, xx] = w[ok] @ no[tri[k]]; hit[yy, xx] = True
    nor /= np.linalg.norm(nor, axis=2, keepdims=True)+1e-9
    return pos, nor, hit
