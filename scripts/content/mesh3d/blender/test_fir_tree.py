"""Spruce A as fir.py writes it without Blender: the size of the intent, the budget, the crown's density, the
boughs' planes and the cover of the branches' axes, and the layout of COLOR_0 the engine's tree wind reads
(reports/content/2026-11-trees/fir/README.md, «Модель»)."""

import json
from pathlib import Path

import numpy as np
import pytest

import fir
import fir_tree
import tree_density
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
    assert metrics['triangles'] <= config['budget_tris'] == 50000
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


def test_no_bough_is_one_plane_and_the_branches_axes_are_under_shoots(spruce, config):
    planes, cover = spruce['metrics']['card_planes'], spruce['metrics']['axis_cover']
    assert set(planes) == {'end', 'roof', 'hang', 'side', 'sleeve', 'leader'}
    for role, numbers in planes.items():
        assert numbers['count'] > 0
        assert numbers['spread_min_deg'] >= (45 if role in ('end', 'hang') else 20), role
    assert config['cards']['cover_from'] == .3
    assert cover['samples'] > 1000 and cover['covered'] >= .85
    assert cover['branch_min'] >= .5


def test_a_flat_card_and_a_bare_branch_are_measured_as_such():
    position = np.array([(0, 0, 0), (1, .3, 0), (1, -.3, 0), (2, 0, 0)], np.float32)
    flat = dict(cutout=True, position=position, index=np.array([(0, 1, 2), (1, 3, 2)]),
                uv=np.zeros((4, 2), np.float32), owner=np.zeros(4, np.int64))

    def tree(*axis):
        branch = fir_tree.Branch(0, np.array(axis, np.float64), np.array([.02, .01]), 1.6, 0., .5, 0.)
        return fir_tree.Tree([dict(cutout=False), flat], dict(scale=1.), [branch],
                             [dict(role='hang', kind='hang_1', branch=0, first=0, count=4)])

    under, beside = tree((.2, 0, -.2), (1.8, 0, -.2)), tree((.2, .5, -.2), (1.8, .5, -.2))
    assert fir_tree.card_planes(under)['hang'] == dict(count=1, spread_min_deg=0.)
    solid = np.ones((8, 8), bool)
    assert fir_tree.axis_cover(under, solid, 0.)['covered'] == 1.
    assert fir_tree.axis_cover(beside, solid, 0.) == dict(start=0., step_m=.1, samples=16, covered=0., branch_min=0.)
    assert fir_tree.axis_cover(under, ~solid, 0.)['covered'] == 0.


def test_the_outline_takes_in_a_hole_and_leaves_out_a_notch_wider_than_the_closing_disc():
    solid = np.zeros((240, 240), bool)
    solid[60:180, 60:180] = True
    hole, notch = (slice(100, 120), slice(100, 120)), (slice(60, 100), slice(140, 154))
    solid[hole] = solid[notch] = False
    inside = tree_density.outline(solid, tree_density.PX, tree_density.CLOSE)
    assert inside[solid].all() and inside[hole].all()
    assert not inside[:60].any() and inside[notch].mean() < .1
    assert 400 <= (inside & ~solid).sum() <= 400 + .1 * 40 * 14


def test_colour_0_is_laid_out_for_the_tree_wind(spruce):
    layout = spruce['metrics']['colour_0']
    assert layout['colour_format'] == 'VEC4 UNSIGNED_BYTE normalized'
    assert layout['trunk']['rgb_max'] == 0 and layout['trunk']['a_min'] == 255
    branches, cards = layout['branches'], layout['cards']
    assert branches['phases'] == branches['count'] > 200
    assert branches['mixed_phase'] == branches['with_b'] == branches['r_not_0_at_trunk'] == 0
    assert branches['r_growth_corr_min'] > .99
    assert cards['unpaired'] == cards['wrong_phase'] == cards['without_one_stem'] == 0
    assert cards['without_free_edge_255'] == cards['without_b_between'] == 0
    assert cards['pairs'] * 2 == cards['triangles']
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
