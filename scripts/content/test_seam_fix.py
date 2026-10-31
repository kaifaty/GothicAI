"""Minimum cuts remove known wrap discontinuities without blending patches."""

import itertools
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).parent))
from seam_fix import _minimum_path, seamless


@pytest.mark.parametrize('closed', [False, True])
def test_dynamic_cut_matches_exhaustive_minimum(closed):
    error = np.array([[8., 2, 9, 5], [3, 1, 4, 8], [7, 6, 2, 4], [1, 8, 3, 9]])
    candidates = [path for path in itertools.product(range(4), repeat=4)
                  if all(abs(a - b) <= 1 for a, b in zip(path, path[1:]))
                  and (not closed or abs(path[-1] - path[0]) <= 1)]
    expected = min(sum(error[row, column] for row, column in enumerate(path))
                   for path in candidates)
    path, total = _minimum_path(error, closed=closed)
    assert total == expected
    assert total == sum(error[row, column] for row, column in enumerate(path))
    assert np.all(np.abs(np.diff(path)) <= 1)
    assert not closed or abs(path[-1] - path[0]) <= 1


def periodic_source(width=96, height=80, overlap_fraction=.125):
    period_u = width - round(width * overlap_fraction)
    period_v = height - round(height * overlap_fraction)
    x = np.arange(width)[None] * 2 * np.pi / period_u
    y = np.arange(height)[:, None] * 2 * np.pi / period_v
    rgb = np.stack(np.broadcast_arrays(128 + 90 * np.sin(x), 128 + 90 * np.sin(y),
                                      128 + 35 * np.sin(x) + 35 * np.sin(y)), axis=-1)
    return Image.fromarray(np.rint(rgb).astype(np.uint8))


def edge_jump(image, axis):
    pixels = np.asarray(image, dtype=float)
    if axis == 'u':
        return np.abs(pixels[:, 0] - pixels[:, -1]).mean()
    return np.abs(pixels[0] - pixels[-1]).mean()


def test_both_joins_improve_on_known_periodic_source_and_metadata_is_json():
    source = periodic_source()
    result, metadata = seamless(source)
    assert result.mode == 'RGB'
    assert result.size == source.size
    for axis in 'uv':
        assert edge_jump(result, axis) < edge_jump(source, axis) / 4
    assert metadata['axes'] == 'uv' and metadata['applied']
    assert metadata['source_size'] == metadata['output_size'] == [96, 80]
    assert [operation['axis'] for operation in metadata['operations']] == ['u', 'v']
    assert [operation['closed_path'] for operation in metadata['operations']] == [False, True]
    assert len(metadata['operations'][0]['path']) == 80
    assert len(metadata['operations'][1]['path']) == 96
    json.dumps(metadata, allow_nan=False)


@pytest.mark.parametrize('axis', ['u', 'v'])
def test_strip_fix_does_not_change_coordinate_on_other_axis(axis):
    source = periodic_source()
    pixels = np.asarray(source).copy()
    if axis == 'u':
        pixels[..., 1] = np.arange(pixels.shape[0], dtype=np.uint8)[:, None] * 3
        channel = 1
    else:
        pixels[..., 0] = np.arange(pixels.shape[1], dtype=np.uint8)[None] * 2
        channel = 0
    result, metadata = seamless(Image.fromarray(pixels), axes=axis)
    np.testing.assert_array_equal(np.asarray(result)[..., channel], pixels[..., channel])
    assert edge_jump(result, axis) < edge_jump(Image.fromarray(pixels), axis) / 4
    assert [operation['axis'] for operation in metadata['operations']] == [axis]


def test_panel_is_unchanged_and_source_is_never_mutated():
    pixels = np.random.default_rng(7).integers(0, 256, size=(13, 19, 3), dtype=np.uint8)
    source = Image.fromarray(pixels)
    result, metadata = seamless(source, axes='')
    assert result is not source
    np.testing.assert_array_equal(result, pixels)
    assert not metadata['applied'] and metadata['operations'] == []
    seamless(source, axes='uv')
    np.testing.assert_array_equal(source, pixels)


@pytest.mark.parametrize('axes', ['u', 'v', 'uv', ''])
def test_constant_is_preserved_exactly_even_after_resize(axes):
    source = Image.new('RGB', (31, 22), (19, 89, 131))
    result, _ = seamless(source, axes=axes)
    np.testing.assert_array_equal(result, source)


@pytest.mark.parametrize('kwargs', [dict(axes='vu'), dict(axes='x'), dict(axes='uu'),
                                   dict(overlap_fraction=0), dict(overlap_fraction=.6),
                                   dict(overlap_fraction=float('nan'))])
def test_invalid_configuration_is_rejected(kwargs):
    with pytest.raises(ValueError):
        seamless(Image.new('RGB', (32, 32)), **kwargs)


def test_non_rgb_and_too_small_corrected_axis_are_rejected():
    with pytest.raises(ValueError, match='RGB'):
        seamless(Image.new('RGBA', (32, 32)))
    with pytest.raises(ValueError, match='6 pixels'):
        seamless(Image.new('RGB', (3, 20)), axes='u')
    # A panel does not need room for an overlap.
    result, _ = seamless(Image.new('RGB', (1, 1)), axes='')
    assert result.size == (1, 1)
