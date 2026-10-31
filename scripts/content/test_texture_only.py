import numpy as np
from PIL import Image

import texture_only as to


def test_a_strip_fills_its_canvas_and_comes_back():
    rng = np.random.default_rng(0)
    strip = Image.fromarray(rng.integers(0, 255, (256, 2048, 3), dtype=np.uint8))
    layout = to.layout_for(strip.size)
    assert tuple(layout['canvas']) in to.CANVASES and layout['count'] > 1
    canvas = to.stack(strip, layout)
    back = np.asarray(to.unstack(canvas, layout).resize(strip.size, Image.LANCZOS), dtype=float)
    assert back.shape == (256, 2048, 3)
    assert np.abs(back - np.asarray(strip, dtype=float)).mean() < 40        # the same pixels, resampled twice


def test_a_square_texture_is_one_piece():
    layout = to.layout_for((512, 512))
    assert layout['count'] == 1 and layout['canvas'] == [1024, 1024] and layout['origin'] == [0, 0]


def test_a_larger_edit_keeps_its_pixels():
    layout = to.layout_for((1024, 256))
    big = Image.new('RGB', (round(layout['canvas'][0] * 1.225), round(layout['canvas'][1] * 1.225)))
    strip = to.unstack(big, layout)
    assert strip.width > 1024 * 1.2 and abs(strip.width / strip.height - 4) < .05
