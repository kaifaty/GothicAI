"""CPU model of the shared-memory indexing in ssao_blur.comp against a plain separable depth-aware blur.

Checks the horizontal pass: every halo row the vertical pass reads (local -R..8+R-1) is blurred exactly once,
nothing lands outside samples[LANE][LANE], and the result equals the reference blur with clamp-to-edge input.
"""
import numpy as np

R, G, SHARP = 3, 8, 0.8           # KERNEL_RADIUS, gl_WorkGroupSize, blurSharpness
LANE = G + 2 * R + 1
FALLOFF = 1 / (2 * (R * 0.5) ** 2)

def blur(taps):
    """taps: (ao, depth) for offsets -R..R; the shader's spatial weight is the same for every tap."""
    c0, d0 = taps[R]
    ct, wt = c0, 1.0
    for k, (c, d) in enumerate(taps):
        if k == R:
            continue
        dd = (d - d0) * SHARP
        w = 2.0 ** (-R * R * FALLOFF - dd * dd)
        ct += w * c
        wt += w
    return np.clip(ct / wt, 0, 1), d0

def reference(C, D):
    H, W = C.shape
    px = lambda y, x: (C[min(max(y, 0), H - 1), min(max(x, 0), W - 1)], D[min(max(y, 0), H - 1), min(max(x, 0), W - 1)])
    hor = {(y, x): blur([px(y, x + k) for k in range(-R, R + 1)]) for y in range(-R, H + R) for x in range(W)}
    return np.array([[blur([hor[y + k, x] for k in range(-R, R + 1)])[0] for x in range(W)] for y in range(H)])

def shader(C, D):
    H, W = C.shape
    px = lambda y, x: (C[min(max(y, 0), H - 1), min(max(x, 0), W - 1)], D[min(max(y, 0), H - 1), min(max(x, 0), W - 1)])
    out = np.zeros((H, W))
    for gy in range(0, H, G):
        for gx in range(0, W, G):
            s = [[px(gy + r - R, gx + i - R) for r in range(LANE)] for i in range(LANE)]   # samples[x][y]
            at = lambda x, y: s[x + R][y + R]
            pending = []
            for ly in range(G):
                for lx in range(G):
                    a0 = (lx, ly - R)
                    a1 = (lx, ly - R + G)
                    rows = [a0] + ([a1] if a1[1] < G + R else [])
                    pending += [(a, blur([at(a[0] + k, a[1]) for k in range(-R, R + 1)])) for a in rows]
            written = [a for a, _ in pending]                       # barrier(), then the writes
            assert len(written) == len(set(written))
            assert all(0 <= a[0] + R < LANE and 0 <= a[1] + R < LANE for a in written)
            assert {(lx, y) for lx in range(G) for y in range(-R, G + R)} <= set(written)
            for (x, y), h in pending:
                s[x + R][y + R] = h
            for ly in range(G):
                for lx in range(G):
                    if gy + ly < H and gx + lx < W:
                        out[gy + ly, gx + lx] = blur([at(lx, ly + k) for k in range(-R, R + 1)])[0]
    return out

if __name__ == "__main__":
    rng = np.random.default_rng(1)
    C, D = rng.random((21, 19)), rng.random((21, 19)) * 3     # sizes not divisible by 8: partial edge groups
    print("max |shader - reference| =", np.abs(shader(C, D) - reference(C, D)).max())
