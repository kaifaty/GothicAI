"""The forest of the cost stand (scripts/content/fir_set.py forest, reports/engine/2026-11-16-modern-trees/tree-cost.md):
the forests of 64, 256 and 1 024 spruces nest, every place keeps its spruce and yaw, the grid is STEP apart."""
import numpy as np
import pytest

import fir_set


def test_the_forests_nest_and_keep_their_places():
    forests = {n: {(t['column'], t['row']): t for t in fir_set.forest_layout(n)} for n in (64, 256, 1024)}
    assert [len(f) for f in forests.values()] == [64, 256, 1024]
    assert set(forests[64]) <= set(forests[256]) <= set(forests[1024])
    for n in (64, 256):
        for key, place in forests[n].items():
            assert place == forests[1024][key]
    assert {c for c, _ in forests[64]} == set(range(8)) and {r for _, r in forests[64]} == set(range(4, 12))
    places = np.array([(t['x'], t['z']) for t in forests[1024].values()])
    cells = np.array(list(forests[1024]))
    centre = np.array([fir_set.X0, fir_set.Z0]) + (cells + .5) * fir_set.STEP
    assert np.abs(places - centre).max() <= fir_set.JITTER
    assert fir_set.forest_layout(0) == []


@pytest.mark.parametrize('count', [10, 49, 2048])
def test_a_forest_is_an_even_square_within_the_grid(count):
    with pytest.raises(SystemExit):
        fir_set.forest_layout(count)
