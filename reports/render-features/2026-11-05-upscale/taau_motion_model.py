"""A numeric model of taau_resolve.frag on a picture that slides at a constant speed (graphics plan 1г, step-1g-1-taau.md,
the third round): how much of the detail survives the resolve in motion.

    uv run --with numpy --with pillow python -I taau_motion_model.py <100 % still of upscale-walk.bench> <variant> ...

The truth is the cobbles of the still, moved by v output pixels a frame; the input is its point samples at 50 % with
the resolve's jitter; the history is read by a Keys cubic at the constant shift. For each variant and speed: the
gradient energy and the energy of four bands of detail against the truth, and the RMS difference with it (0..255),
over the last eight frames. Variants: `base` is the second round, `noclip` the same without the colour box, `Cxx`
a history cubic with deeper lobes, `enl-sNN` the enlarged frame with a least share in motion, and their mixes.
It has no RCAS and no tonemapping, so only the differences between variants mean something.
"""
import sys
import numpy as np
from PIL import Image

BOX = (500, 1250, 1012, 1506)

def halton(i, b):
    f, r = 1.0, 0.0
    while i > 0:
        f /= b; r += f*(i % b); i //= b
    return r

def fshift(H, dx, dy):
    h, w = H.shape
    fx = np.fft.fftfreq(w)[None, :]; fy = np.fft.fftfreq(h)[:, None]
    return np.fft.ifft2(np.fft.fft2(H)*np.exp(-2j*np.pi*(fx*dx + fy*dy))).real

def keys(x, C):
    x = np.abs(x)
    return np.where(x < 1, (2 - C)*x**3 - (3 - C)*x**2 + 1, np.where(x < 2, -C*x**3 + 5*C*x**2 - 8*C*x + 4*C, 0.0))

def resample(A, sx, sy, C=0.5):
    out = A
    for axis, s in ((1, sx), (0, sy)):
        i0 = int(np.floor(s)); f = s - i0
        w = [keys(f + 1, C), keys(f, C), keys(1 - f, C), keys(2 - f, C)]
        out = sum(wk*np.roll(out, -(i0 + k - 1), axis=axis) for k, wk in enumerate(w))
    return out

def enlarge(I, j, w, h, C):
    # the input's samples lie at input coordinate c + 0.5 - j; the output pixel n looks at (n + 0.5)/2 (input px)
    def ax(A, n, jj, axis):
        p = (np.arange(n) + 0.5)/2 + jj - 0.5
        i0 = np.floor(p).astype(int); f = p - i0
        ws = [keys(f + 1, C), keys(f, C), keys(1 - f, C), keys(2 - f, C)]
        N = A.shape[axis]
        if axis == 1: return sum(ws[k][None, :]*A[:, (i0 + k - 1) % N] for k in range(4))
        return sum(ws[k][:, None]*A[(i0 + k - 1) % N, :] for k in range(4))
    return ax(ax(I, w, j[0], 1), h, j[1], 0)

def blur(f, r):
    k = 2*r + 1
    c = np.cumsum(np.pad(f, ((r+1, r), (0, 0)), mode="wrap"), axis=0); f = (c[k:] - c[:-k])/k
    c = np.cumsum(np.pad(f, ((0, 0), (r+1, r)), mode="wrap"), axis=1); return (c[:, k:] - c[:, :-k])/k

def bands(f, r):
    def b(x, lo, hi): return ((blur(x, lo) if lo else x) - blur(x, hi)).std()
    sh = lambda x: np.abs(np.diff(x, axis=1)).mean() + np.abs(np.diff(x, axis=0)).mean()
    return (sh(f)/sh(r), b(f,0,1)/b(r,0,1), b(f,1,2)/b(r,1,2), b(f,2,4)/b(r,2,4), b(f,4,8)/b(r,4,8),
            255*np.sqrt(((f - r)**2).mean()))

def run(H, v, frames=44, wMax=4.0, kernel=6.0, clipTrust=4.0, gamma=1.0, boxStatic=3.0, boxMotion=1.0,
        Cm=0.5, mSpeed=None, mShare=0.0, enl=None, kernelM=None, phases=32, gammaM=None):
    h, w = H.shape
    hist = None
    speed = float(np.hypot(*v))
    still = 1 - min(speed/boxMotion, 1.0)
    m = 0.0 if mSpeed is None else min(speed/mSpeed, 1.0)   # 1: in full motion
    C = 0.5 + (Cm - 0.5)*(1 - still)
    yy, xx = np.mgrid[0:h, 0:w]
    errs = []
    for t in range(frames):
        j = np.array([halton(t % phases + 1, 2) - 0.5, halton(t % phases + 1, 3) - 0.5])
        truth = fshift(H, v[0]*t, v[1]*t)
        S = fshift(H, v[0]*t - (0.5 - 2*j[0]), v[1]*t - (0.5 - 2*j[1]))
        I = S[::2, ::2]
        px = (xx + 0.5)/2 + j[0]; py = (yy + 0.5)/2 + j[1]
        cx = np.floor(px).astype(int) % (w//2); cy = np.floor(py).astype(int) % (h//2)
        dx = ((np.floor(px) + 0.5) - px)*2; dy = ((np.floor(py) + 0.5) - py)*2
        kk = kernel if kernelM is None else kernel + (kernelM - kernel)*m
        conf = np.exp(-kk*(dx*dx + dy*dy))
        cur = I[cy, cx]
        if enl is not None and m > 0:
            cur = cur + (enlarge(I, j, w, h, enl) - cur)*m
        if hist is None:
            hist = enlarge(I, j, w, h, 0.5); hA = np.full_like(H, 0.05); continue
        nb = np.stack([np.roll(np.roll(I, a, 0), b, 1) for a in (-1, 0, 1) for b in (-1, 0, 1)])
        mean = nb.mean(0); sig = nb.std(0)
        g = gamma if gammaM is None else gamma + (gammaM - gamma)*m
        vmin = np.maximum(mean - g*sig, nb.min(0)); vmax = np.minimum(mean + g*sig, nb.max(0))
        hr = resample(hist, -v[0], -v[1], C); ar = resample(hA, -v[0], -v[1], 0.0)
        held = np.clip((ar - 0.05)/0.95, 0, 1)
        mid = (0.5*(vmin + vmax))[cy, cx]; ext = (0.5*(vmax - vmin))[cy, cx]*(1 + (boxStatic - 1)*still*held)
        hc = np.clip(hr, mid - ext - 1e-5, mid + ext + 1e-5)
        moved = np.abs(hc - hr)/(ext + 1e-3)
        wH = np.minimum(np.maximum(ar, 0), wMax)/(1 + clipTrust*moved)
        a = conf/(conf + wH + 1e-4)
        a = a + (np.maximum(a, mShare) - a)*m
        wc = a/(1 + cur); wh = (1 - a)/(1 + hc)
        hist = (cur*wc + hc*wh)/np.maximum(wc + wh, 1e-6)
        hA = np.maximum(np.minimum(wH + conf, wMax), 0.05)
        if t >= frames - 8: errs.append(bands(hist, truth))
    return np.mean(errs, axis=0)

H = np.asarray(Image.open(sys.argv[1]).convert("RGB").crop(BOX), dtype=np.float64) @ np.array([0.2126, 0.7152, 0.0722])/255.0
SPEEDS = ((0.0, 0.0), (0.37, 0.61), (2.37, 5.63))
def show(name, **kw):
    for v in SPEEDS:
        r = run(H, v, **kw)
        print(f"{name:26s} v={v!s:13s} sharp {r[0]:4.2f} 1-2 {r[1]:4.2f} 2-4 {r[2]:4.2f} 4-8 {r[3]:4.2f} 8-16 {r[4]:4.2f} rmse {r[5]:5.2f}")
V = {
 "base": dict(),
 "C075": dict(Cm=0.75),
 "C10": dict(Cm=1.0),
 "C125": dict(Cm=1.25),
 "enl-s20": dict(mSpeed=1.0, enl=0.5, mShare=0.2),
 "enl-s35": dict(mSpeed=1.0, enl=0.5, mShare=0.35),
 "C10-enl-s20": dict(Cm=1.0, mSpeed=1.0, enl=0.5, mShare=0.2),
 "C10-enl-s10": dict(Cm=1.0, mSpeed=1.0, enl=0.5, mShare=0.1),
 "C075-enl-s20": dict(Cm=0.75, mSpeed=1.0, enl=0.5, mShare=0.2),
 "C10-enl-s35": dict(Cm=1.0, mSpeed=1.0, enl=0.5, mShare=0.35),
 "C10-k3": dict(Cm=1.0, mSpeed=1.0, kernelM=3.0),
 "C10-g15": dict(Cm=1.0, mSpeed=1.0, gammaM=1.5),
 "C10-box1": dict(Cm=1.0, boxMotion=0.25),
 "C10-bm4": dict(Cm=1.0, boxMotion=4.0),
 "bm4": dict(boxMotion=4.0),
 "noclip": dict(boxStatic=1000.0, boxMotion=1000.0),
 "enl-s100": dict(mSpeed=1.0, enl=0.5, mShare=1.0),
 "enl-s50": dict(mSpeed=1.0, enl=0.5, mShare=0.5),
 "C10-enl-s50": dict(Cm=1.0, mSpeed=1.0, enl=0.5, mShare=0.5),
 "C125-enl-s35": dict(Cm=1.25, mSpeed=1.0, enl=0.5, mShare=0.35),
 "C125-enl-s50": dict(Cm=1.25, mSpeed=1.0, enl=0.5, mShare=0.5),
 "C10-enl75-s35": dict(Cm=1.0, mSpeed=1.0, enl=0.75, mShare=0.35),
 "C10-enl-s35-g15": dict(Cm=1.0, mSpeed=1.0, enl=0.5, mShare=0.35, gammaM=1.5),
 "C10-enl-s35-ct0": dict(Cm=1.0, mSpeed=1.0, enl=0.5, mShare=0.35, clipTrust=0.0),
}
for n in sys.argv[2:]: show(n, **V[n])
