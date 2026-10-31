"""Column-stripe metric: FFT of the row-averaged horizontal high-pass, peak at a period / median."""
import sys
import numpy as np
from PIL import Image

def spectrum(img, axis):
    hp_w = 9
    k = np.ones(hp_w) / hp_w
    # high-pass along `axis`, then average along the other axis
    a = img if axis == 1 else img.T
    lp = np.apply_along_axis(lambda r: np.convolve(r, k, mode="same"), 1, a)
    hp = (a - lp)[:, hp_w:-hp_w]
    sig = hp.mean(axis=0)
    F = np.abs(np.fft.rfft(sig - sig.mean()))
    return F, len(sig)

def peak(F, n, period):
    b = n / period
    i = int(round(b))
    p = F[max(i - 1, 1):i + 2].max()
    med = np.median(F[1:])
    return p / med

def load(path):
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    return im.mean(axis=2)

if __name__ == "__main__":
    print(f"{'file':60s} x5    x10   y5    y8")
    for path in sys.argv[1:]:
        g = load(path)
        Fx, nx = spectrum(g, 1)
        Fy, ny = spectrum(g, 0)
        print(f"{path[-60:]:60s} {peak(Fx,nx,5):5.1f} {peak(Fx,nx,10):5.1f} {peak(Fy,ny,5):5.1f} {peak(Fy,ny,8):5.1f}")
