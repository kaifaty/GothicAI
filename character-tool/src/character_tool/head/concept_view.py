"""The whole views of the concept as a frame to measure in: masks, outlines and distances to them.

The frame is the one of params.json (auto_params.py or by hand): the concept's front and profile, `res` px square,
the middle of the eyes at front_eye / profile_eye, `ipd_px` px between the pupils (61.5 mm). The front looks along
+y (the head's left is on the right), the profile is seen from +x (the face looks left). Unlike the frames of
views.json (24 cm round the face, made for picking landmarks), the whole views hold the crown and the nape.
"""
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage


class Views:
    def __init__(self, params, eye, scale=1):
        """`scale`: masks are made at this many pixels per pixel of the concept."""
        self.k = scale; self.res = params['res'][0]*scale; self.mpp = 0.0615/params['ipd_px']/scale; self.eye = np.asarray(eye, float)
        self.eye_px = {'front': np.array(params['front_eye'], float)*scale, 'profile': np.array(params['profile_eye'], float)*scale}

    def proj(self, p, view):
        """Points in metres -> (u, v) in pixels."""
        a = 0 if view == 'front' else 1; ex, ey = self.eye_px[view]
        return ex+(p[:, a]-self.eye[a])/self.mpp, ey-(p[:, 2]-self.eye[2])/self.mpp

    def row(self, view, z):
        """The row of the height z (metres over the eyes)."""
        return int(round(self.eye_px[view][1]-z/self.mpp))

    def concept(self, path):
        """The figure on the concept: what differs from the background of the top corners, in one piece."""
        im = Image.open(path).convert('RGB')
        if self.k != 1: im = im.resize((self.res, self.res), Image.BICUBIC)
        a = np.asarray(im).astype(float); c = 20*self.k
        bg = np.median(np.r_[a[:c, :c].reshape(-1, 3), a[:c, -c:].reshape(-1, 3)], 0)
        m = ndimage.binary_fill_holes(ndimage.binary_opening(np.abs(a-bg).sum(-1) > 25, iterations=2*self.k)); lab, n = ndimage.label(m)
        return lab == (np.argmax(np.bincount(lab.ravel())[1:])+1) if n else m

    def points(self, p, view, close=2):
        """The outline of a dense cloud of points, filled."""
        u, v = self.proj(p, view); m = np.zeros((self.res, self.res), bool)
        i, k = np.round(v).astype(int), np.round(u).astype(int); ok = (i >= 0) & (i < self.res) & (k >= 0) & (k < self.res); m[i[ok], k[ok]] = True
        return ndimage.binary_fill_holes(ndimage.binary_closing(m, iterations=close*self.k))

    def tris(self, p, tris, view):
        """The outline of a mesh, filled."""
        u, v = self.proj(p, view); im = Image.new('1', (self.res, self.res), 0); d = ImageDraw.Draw(im)
        for t in tris: d.polygon([(float(u[i]), float(v[i])) for i in t], fill=1, outline=1)
        return ndimage.binary_fill_holes(np.asarray(im))

    def sdf(self, m):
        """Signed distance to the outline of a mask, mm: negative inside. Half a pixel is taken off both sides, so the
        outline runs between the last pixel inside and the first outside."""
        o, i = ndimage.distance_transform_edt(~m), ndimage.distance_transform_edt(m)
        return (np.where(m, -(i-0.5), o-0.5))*self.mpp*1000

    @staticmethod
    def edge(m):
        return m & ~ndimage.binary_erosion(m)

    @staticmethod
    def at(f, u, v):
        """A field at (u, v), bilinear."""
        return ndimage.map_coordinates(f, [v, u], order=1, mode='nearest')

    def by_angle(self, m, sd, view, low, step=15, centre=(0.085, -0.02)):
        """How far the outline of mask `m` stands from the outline whose signed distance is `sd` (positive: outside
        it), by the direction from the head's middle: {degrees: median mm}. 0 is +u (the front view: the head's
        left; the profile: the back), 90 is up. Only above `low` metres over the eyes."""
        ex, ey = self.eye_px[view]; cu = ex+(centre[0]/self.mpp if view == 'profile' else 0); cv = ey-centre[1]/self.mpp
        i, k = np.nonzero(self.edge(m)); ok = i < self.row(view, low); i, k = i[ok], k[ok]; d = sd[i, k]
        ang = np.degrees(np.arctan2(-(i-cv), k-cu)) % 360; out = {}
        for a0 in range(0, 360, step):
            s = (ang >= a0) & (ang < a0+step)
            if s.sum() > 5*self.k: out[a0] = round(float(np.median(d[s])), 2)
        return out
