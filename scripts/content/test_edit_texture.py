import numpy as np

import edit_texture as et
import timber_bake as tb


def _pair(seed=0, size=256):
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:size, 0:size] / size
    broad = .3 + .2 * np.sin(2 * np.pi * xx)[..., None] * np.array([1., .8, .5])
    original = np.clip(broad + .02 * rng.standard_normal((size, size, 3)), 0, 1)
    grain = 1 + .3 * rng.standard_normal((size, size))
    edit = np.clip(broad * np.array([1.2, 1.1, .7]) * grain[..., None], 0, 1)    # warm drift, sharper grain
    return edit, original


def test_colour_from_the_original_detail_from_the_edit():
    edit, original = _pair()
    out = et.remaster(edit, original, (1., 1.))['albedo']
    assert np.allclose(out.mean(axis=(0, 1)), original.mean(axis=(0, 1)), rtol=.06)   # the drift is gone
    from scipy.ndimage import gaussian_filter
    fine = lambda image: (lambda l: (l - gaussian_filter(l, 4, mode='wrap')).std())(image @ tb.LUM)
    assert fine(out) > fine(original) * 2                                                  # the edit's detail stays


def test_relief_reaches_the_asked_tilt():
    edit, original = _pair(1)
    for tilt in (8., 14.):
        normal = et.remaster(edit, original, (1., 1.), tilt)['normal']
        assert abs(et._median_tilt(normal) - tilt) < 1.


def test_a_dark_original_pixel_gives_no_coloured_rim():
    edit, original = _pair(2)
    original[100:104, :, :] = np.array([.004, .0005, .0005])     # a dark joint whose hue is all red noise
    edit[100:104, :, :] = .3                                     # the edit puts light stone there
    albedo = et.remaster(edit, original, (1., 1.))['albedo']
    rim = albedo[100:104]
    assert (rim[..., 0] / np.maximum(rim[..., 1], 1e-6)).mean() < 2.     # not pushed to red


def test_relief_blur_smooths_the_finest_band():
    edit, original = _pair(3)
    sharp = et.remaster(edit, original, (1., 1.))['normal']
    soft = et.remaster(edit, original, (1., 1.), relief_blur=.02)['normal']
    step = lambda n: np.abs(np.diff(n[..., 0], axis=1)).mean()
    assert step(soft) < step(sharp) * .7
