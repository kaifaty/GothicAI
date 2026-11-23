"""Which depth pixel the old half-res SSAO read for each AO column (float32, IEEE division).

ssao.comp put the AO texel x at NDC (2x+1)/(W/2) - 1, which is exactly the border between depth
pixels 2x and 2x+1. Nearest sampling picks one of them by the rounding error. At W=1280 the error
depends on x mod 5 (640 = 5 * 128), so the pick repeats every 5 AO texels = 10 screen pixels and
its harmonics land at 5 and 3.33 px. At W=1024 the division is exact and the pick never changes.

    python3 rounding.py
"""
import numpy as np

f = np.float32


def picks(width):
    half = f(width // 2)
    x = np.arange(width // 2, dtype=np.float32)
    in_pos = (f(2) * x + f(1)) / half - f(1)
    texel = (in_pos * f(0.5) + f(0.5)) * f(width)
    return np.floor(texel) - 2 * x  # 0: left pixel, 1: right pixel


for width in (1280, 1024, 1920, 3456):
    p = picks(width)
    spectrum = np.abs(np.fft.rfft(p - p.mean()))
    freq = np.fft.rfftfreq(len(p))
    spectrum[freq < 2 / 50] = 0  # keep screen periods under 50 px: the slow drift is not a stripe
    top = np.argsort(spectrum)[::-1][:3]
    periods = ", ".join(f"{2 / freq[i]:.2f} px" for i in top if spectrum[i] > 1e-6) or "none"
    print(f"{width}: left pick {1 - p.mean():.0%}, strongest screen periods: {periods}")
