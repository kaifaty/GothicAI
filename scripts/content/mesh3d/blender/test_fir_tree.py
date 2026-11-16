"""Spruce A as fir.py writes it without Blender: the size of the intent, the budget, the crown's density and the
layout of COLOR_0 the engine's tree wind reads (reports/content/2026-11-trees/fir/README.md, «Модель»)."""

import json
from pathlib import Path

import pytest

import fir
import tree_glb

HERE = Path(__file__).resolve().parent


@pytest.fixture(scope='module')
def config():
    return json.loads((HERE / 'fir.json').read_text())


@pytest.fixture(scope='module')
def spruce(config, tmp_path_factory):
    return fir.build(config, tmp_path_factory.mktemp('fir-a'), 'fir-a')


def test_spruce_a_has_the_size_of_the_intent_within_the_budget(spruce, config):
    metrics = spruce['metrics']
    assert metrics['fails'] == []
    assert metrics['triangles'] <= config['budget_tris'] == 45000
    assert 17.5 <= metrics['height_m'] <= 18.5
    assert .5 <= metrics['trunk_diameter_m'][1] <= .7 < metrics['trunk_diameter_m'][0]
    assert metrics['inside_out_parts'] == 0
    assert metrics['bbox_min_m'][2] < 0 < 17.5 < metrics['bbox_max_m'][2]


def test_the_crown_is_dense_from_both_sides(spruce):
    density = spruce['metrics']['density']
    assert [view['view'] for view in density['views']] == ['along Y', 'along X']
    for view in density['views']:
        assert view['background_share'] <= .15
        assert view['trunk_open_length_share'] <= .1
        assert 9. <= view['crown_width_m'] <= 10.


def test_colour_0_is_laid_out_for_the_tree_wind(spruce):
    layout = spruce['metrics']['colour_0']
    assert layout['colour_format'] == 'VEC4 UNSIGNED_BYTE normalized'
    assert layout['trunk']['rgb_max'] == 0 and layout['trunk']['a_min'] == 255
    branches, cards = layout['branches'], layout['cards']
    assert branches['phases'] == branches['count'] > 200
    assert branches['mixed_phase'] == branches['with_b'] == branches['r_not_0_at_trunk'] == 0
    assert branches['r_growth_corr_min'] > .99
    assert cards['unpaired'] == cards['wrong_phase'] == cards['without_one_stem'] == 0
    assert cards['without_free_edge_255'] == 0 and cards['pairs'] * 2 == cards['triangles']
    assert layout['openness']['corr_with_radius_share'] > .9
    wood, boughs = tree_glb.read(spruce['glb'])
    assert (wood['material']['name'], boughs['material']['name']) == ('OWN_TREE_FIR_BARK', 'OWN_TREE_FIR_BOUGHS')
    assert wood['material'].get('alphaMode') != 'MASK' and boughs['material']['alphaMode'] == 'MASK'
    assert wood['colour'].shape[1] == boughs['colour'].shape[1] == 4


def test_a_seed_and_a_scale_give_another_spruce_of_the_same_kind(config, spruce, tmp_path):
    other = fir.build(config, tmp_path, 'fir-b', seed=48, scale=.8)['metrics']
    assert other['fails'] == []
    assert other['height_m'] == pytest.approx(.8 * 18, abs=.5)
    assert other['triangles'] != spruce['metrics']['triangles']
    assert other['density']['background_share'] <= .15
