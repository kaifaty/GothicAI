"""Spruce A as fir.py writes it without Blender: the size of the intent, the budget, the crown's density, the
boughs' planes, the clusters of shoots and the cover of the branches' axes, the layout of COLOR_0 the engine's tree wind reads
(reports/content/2026-11-trees/fir/README.md, «Модель») and the cards' sides: once under a two-sided material, or
twice in the mode of pairs (reports/engine/2026-11-16-modern-trees/two-sided.md)."""

import copy
import json
from pathlib import Path

import numpy as np
import pytest

import fir
import fir_tree
import tree_density
import tree_glb
import tree_prefix

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
    assert metrics['triangles'] <= config['budget_tris'] == 48000
    assert 44900 <= metrics['triangles'] <= 45400       # a bough's triangle is drawn once: 83 912 with the twins
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
    assert set(planes) == {'end', 'tip', 'roof', 'hang', 'side', 'sleeve', 'leader'}
    for role, numbers in planes.items():
        assert numbers['count'] > 0
        assert numbers['spread_min_deg'] >= dict(end=45, hang=30, tip=25).get(role, 20), role
    assert planes['end']['blades_min'] == 3             # two wings and the blade across them
    assert config['cards']['cover_from'] == .3
    assert cover['samples'] > 1000 and cover['covered'] >= .9
    assert cover['branch_min'] >= .5


def test_a_hanging_bough_and_a_branchs_end_are_clusters_of_shoots(spruce):
    """The rule of the clusters: a hanging bough is 3 to 5 shoots, a branch's end 3 or 4, each shoot a bough of
    its own. In every cluster two shoots lie in planes 25 degrees apart or more (10 on an end, whose leader has
    three blades) and two spines go 20 degrees apart or more (45 on an end): no side shows it as one sheet."""
    planes, found = spruce['metrics']['card_planes'], spruce['metrics']['clusters']
    hang, end = found['hang'], found['end']
    assert hang['count'] > 900 and hang['shoots'] == [3, 5] and 3.5 <= hang['shoots_mean'] <= 4.2
    assert hang['planes_min_deg'] >= 25 and hang['splay_min_deg'] >= 20
    assert end['count'] == planes['end']['count'] > 200 and end['shoots'] == [3, 4]
    assert end['planes_min_deg'] >= 10 and end['splay_min_deg'] >= 45
    assert planes['hang']['count'] == hang['shoots_total']         # no hanging bough is left a single card
    assert planes['tip']['count'] == end['shoots_total'] - end['count']
    cards = spruce['metrics']['colour_0']['cards']
    assert cards['count'] == sum(role['count'] for role in planes.values()) and cards['without_one_stem'] == 0


def test_a_roof_sunk_into_the_wood_and_an_end_of_two_wings_break_the_rules(spruce, config, monkeypatch):
    mask = tree_density.load_mask(HERE / config['atlas']['mask'])
    frond = fir_tree.Fir.frond
    assert spruce['metrics']['fails'] == []

    def sunk(self, *args, **kwargs):
        return frond(self, *args, **kwargs | (dict(rise=0.) if args[8] in ('roof', 'end') else {}))

    def two_wings(self, *args, **kwargs):
        return frond(self, *args, **kwargs | (dict(keel=0) if args[8] == 'end' else {}))

    on_the_axis = copy.deepcopy(config)
    on_the_axis['cards']['lift'] = 0.
    monkeypatch.setattr(fir_tree.Fir, 'frond', sunk)
    measured, fails = fir.card_rules(fir_tree.build(on_the_axis), on_the_axis, mask)
    assert measured['axis_cover']['covered'] < .86
    assert [text for text in fails if "of the branches' axes is under boughs" in text]
    monkeypatch.setattr(fir_tree.Fir, 'frond', two_wings)
    measured, fails = fir.card_rules(fir_tree.build(config), config, mask)
    assert measured['card_planes']['end']['blades_min'] == 2
    assert fails == ['a bough of the role end has 2 blades, the least is 3']


def test_a_flat_card_and_a_bare_branch_are_measured_as_such():
    position = np.array([(0, 0, 0), (1, .3, 0), (1, -.3, 0), (2, 0, 0)], np.float32)
    flat = dict(cutout=True, position=position, index=np.array([(0, 1, 2), (1, 3, 2)]),
                uv=np.zeros((4, 2), np.float32), owner=np.zeros(4, np.int64))

    def tree(*axis):
        branch = fir_tree.Branch(0, np.array(axis, np.float64), np.array([.02, .01]), 1.6, 0., .5, 0.)
        return fir_tree.Tree([dict(cutout=False), flat], dict(scale=1.), [branch],
                             [dict(role='hang', kind='hang_1', branch=0, first=0, count=4)])

    under, beside = tree((.2, 0, -.2), (1.8, 0, -.2)), tree((.2, .5, -.2), (1.8, .5, -.2))
    assert fir_tree.card_planes(under)['hang'] == dict(count=1, spread_min_deg=0., blades_min=1)
    assert fir_tree.clusters(under) == {}
    solid = np.ones((8, 8), bool)
    assert fir_tree.axis_cover(under, solid, 0.)['covered'] == 1.
    assert fir_tree.axis_cover(beside, solid, 0.) == dict(start=0., step_m=.1, samples=16, covered=0., branch_min=0.)
    assert fir_tree.axis_cover(under, ~solid, 0.)['covered'] == 0.
    # the card over the axis's centre but inside the wood, and on the bark without the clearance: the axis is open
    for depth in (-.005, -.018):
        assert fir_tree.axis_cover(tree((.2, 0, depth), (1.8, 0, depth)), solid, 0.)['covered'] == 0.
    assert fir_tree.axis_cover(tree((.2, 0, -.035), (1.8, 0, -.035)), solid, 0.)['covered'] == 1.


def test_the_outline_takes_in_a_hole_and_leaves_out_a_notch_wider_than_the_closing_disc():
    solid = np.zeros((240, 240), bool)
    solid[60:180, 60:180] = True
    hole, notch = (slice(100, 120), slice(100, 120)), (slice(60, 100), slice(140, 154))
    solid[hole] = solid[notch] = False
    inside = tree_density.outline(solid, tree_density.PX, tree_density.CLOSE)
    assert inside[solid].all() and inside[hole].all()
    assert not inside[:60].any() and inside[notch].mean() < .1
    assert 400 <= (inside & ~solid).sum() <= 400 + .1 * 40 * 14


def test_ranked_prefix_covers_two_boughs_before_adding_a_duplicate(tmp_path, monkeypatch):
    def quad(left, right, low, high, depth):
        return [(left, depth, low), (left, depth, high), (right, depth, low), (right, depth, high)]

    def part(name, quads, cutout, openness):
        position = np.array(quads, np.float32).reshape(-1, 3)
        count = len(position)
        colour = np.zeros((count, 4), np.uint8)
        colour[:, 2] = np.tile([0, 128, 128, 255], len(quads))
        colour[:, 3] = openness
        index = np.array([(0, 1, 2), (1, 3, 2)]) + np.arange(len(quads))[:, None, None] * 4
        return dict(name=name, cutout=cutout, position=position, normal=np.tile([0, 1, 0], (count, 1)),
                    uv=np.full((count, 2), .5), colour=colour, index=index.reshape(-1, 3))

    trunk = quad(-.06, .06, 0, 3, 0)
    wood = part('bark', [trunk, quad(-.18, -.08, .2, .4, 0)], False, 255)
    level_wood = part('bark', [trunk], False, 255)
    # In the +Y order view the narrow right bough wins on silhouette weight,
    # then the front left bough beats the duplicate with the same footprint.
    left, right = quad(-1.2, -.2, .8, 2, .15), quad(.14, .48, .25, 2.7, .15)
    hidden = quad(-1.2, -.2, .8, 2, .14999)
    monkeypatch.setattr(tree_prefix, 'ORDER_VIEWS', ((0, 0),))
    full, designed = tmp_path / 'full.glb', tmp_path / 'designed.glb'
    tree_glb.write(full, [wood, part('boughs', [right, hidden, left], True, 128)])
    tree_glb.write(designed, [level_wood, part('boughs', [left, right], True, 128)])
    mask = np.ones((4, 4), bool)
    bark, boughs = tree_prefix.split_parts(tree_glb.read(full))
    first, _, by_face = tree_prefix.cards(boughs)
    points = np.concatenate((bark['position'], boughs['position']))
    order = tree_prefix.rank_cards(points, bark, boughs, mask, by_face, np.bincount(by_face, minlength=len(first)))
    assert order.tolist() == [0, 2, 1]

    rows = tree_prefix.measure(full, {50: designed}, mask, np.full((4, 4), 27.), {50: (1/3, 2/3)})[50]
    assert (rows['same_total']['cards'], rows['same_total']['wood_triangles']) == (1, 4)
    assert (rows['same_canopy']['cards'], rows['same_canopy']['wood_triangles']) == (2, 2)
    assert rows['same_total']['triangles'] == rows['same_canopy']['triangles'] == rows['designed']['triangles'] == 6
    assert rows['same_total']['views'] == rows['0.333333']['views']
    assert rows['same_canopy']['views'] == rows['designed']['views']
    assert rows['0.333333']['views'][0]['reference_opening'] > rows['0.666667']['views'][0]['reference_opening']
    assert rows['0.666667_grown']['growth'] == pytest.approx(1.5**.25)
    assert rows['0.666667_grown']['views'][0]['extra_outline'] > 0


def test_prefix_render_samples_atlas_and_interpolates_openness():
    leaves = dict(position=np.array([(0, 0, 0), (6, 0, 0), (0, 6, 0)]),
                  index=np.array([(0, 1, 2)]), uv=np.array([(0, 0), (1, 0), (0, 1)]),
                  colour=np.array([(0, 0, 0, 0), (0, 0, 128, 255), (0, 0, 255, 0)], np.uint8))
    mask = np.array([[False, True], [False, False]])
    luma = np.array([[1, 11, 21], [31, 41, 51], [61, 71, 81]])
    _, tone = tree_prefix.render(leaves, lambda points: points, (6, 6), np.full(36, np.inf), mask, luma)
    # The six hits sample luma 11, 11, 41, 21, 21 and 21. Their interpolated A is x/6
    # for x = 3.5, 4.5 and 5.5, hit three, two and one times respectively.
    assert tone == pytest.approx([126, 25 / 6, 6])


def test_colour_0_is_laid_out_for_the_tree_wind(spruce):
    layout = spruce['metrics']['colour_0']
    assert layout['colour_format'] == 'VEC4 UNSIGNED_BYTE normalized'
    assert layout['trunk']['rgb_max'] == 0 and layout['trunk']['a_min'] == 255
    branches, cards = layout['branches'], layout['cards']
    assert branches['phases'] == branches['count'] > 200
    assert branches['mixed_phase'] == branches['with_b'] == branches['r_not_0_at_trunk'] == 0
    assert branches['r_growth_corr_min'] > .99
    assert cards['wrong_phase'] == cards['without_one_stem'] == 0
    assert cards['without_free_edge_255'] == cards['without_b_between'] == 0
    assert cards['sides'] == 'material' and cards['pairs'] == 0 and cards['single'] == cards['triangles']
    assert layout['openness']['corr_with_radius_share'] > .9
    wood, boughs = tree_glb.read(spruce['glb'])
    assert (wood['material']['name'], boughs['material']['name']) == ('OWN_TREE_FIR_BARK', 'OWN_TREE_FIR_BOUGHS')
    assert wood['material'].get('alphaMode') != 'MASK' and boughs['material']['alphaMode'] == 'MASK'
    assert boughs['material']['doubleSided'] and not wood['material']['doubleSided']
    assert wood['colour'].shape[1] == boughs['colour'].shape[1] == 4


def test_a_seed_and_a_scale_give_another_spruce_of_the_same_kind(config, spruce, tmp_path):
    other = fir.build(config, tmp_path, 'fir-b', seed=48, scale=.8)['metrics']
    assert other['fails'] == []
    assert other['height_m'] == pytest.approx(.8 * 18, abs=.5)
    assert other['triangles'] != spruce['metrics']['triangles']
    assert other['density']['background_share'] <= .15


def test_the_mode_of_pairs_only_adds_the_twins(config, spruce, tmp_path):
    pairs = fir.build(config, tmp_path, 'fir-pairs', pairs=True)
    metrics, single = pairs['metrics'], spruce['metrics']
    assert metrics['fails'] == []
    cards = metrics['colour_0']['cards']
    assert cards['sides'] == 'pairs' and cards['single'] == 0
    assert cards['pairs'] == single['colour_0']['cards']['triangles'] == cards['triangles'] // 2
    assert metrics['triangles'] == single['triangles'] + cards['pairs'] == 83912
    for name in ('density', 'card_planes', 'clusters', 'axis_cover'):
        assert metrics[name] == single[name]
    (_, twice), (_, once) = tree_glb.read(pairs['glb']), tree_glb.read(spruce['glb'])
    assert not twice['material']['doubleSided']
    for name in ('position', 'normal', 'uv', 'colour'):
        assert np.array_equal(twice[name], once[name])


def digest(tree) -> str:
    """The arrays of a built tree at a millimetre: the same spruce on every platform gives the same digest."""
    import hashlib
    h = hashlib.sha256()
    for p in tree.parts:
        h.update(np.rint(p['position'] * 1000).astype(np.int64).tobytes())
        h.update(p['index'].astype(np.int64).tobytes())
        h.update(np.rint(p['uv'] * 1e4).astype(np.int64).tobytes())
        h.update(p['colour'].tobytes())
    return h.hexdigest()


def test_spruce_a_is_built_as_before_and_thin_is_a_separate_step(config):
    """thin() of the cost stand (reports/engine/2026-11-16-modern-trees/tree-cost.md) does not touch spruce A:
    the digest is that of the generator before it; share 1 gives the tree itself."""
    tree = fir_tree.build(config)
    assert digest(tree) == 'c6e92de4460a137d725f6f97a5442c30865f6c786054bf300da110c85ef9d79b'
    assert fir_tree.thin(tree, 1.) is tree
    full_width = sum(tree.metrics['crown_width_m'])
    for share in (.5, .25, .125):
        thinned = fir_tree.thin(tree, share)
        assert abs(thinned.metrics['triangles'] / tree.metrics['triangles'] - share) < .01
        assert abs(sum(thinned.metrics['crown_width_m']) / full_width - 1) < .03
        assert thinned.metrics['thin']['card_growth'] == pytest.approx(thinned.metrics['thin']['cards_share'] ** -.5,
                                                                       abs=1e-3)
    assert digest(fir_tree.build(config)) == digest(tree)


@pytest.mark.parametrize(('px', 'old_share'), [(400, .5), (200, .25), (100, .125), (50, 1 / 32)])
def test_designed_levels_preserve_the_crown_and_the_source_cards_wind(spruce, config, px, old_share):
    tree = spruce['tree']
    before = digest(tree)
    normals = [part['normal'].copy() for part in tree.parts]
    mask = tree_density.load_mask(HERE / config['atlas']['mask'])
    _, boughs = tree.parts
    source_colour = boughs['colour']
    starts = [card['first'] for card in tree.cards]
    positions = boughs['position'][boughs['index']]
    areas = np.linalg.norm(np.cross(positions[:, 1] - positions[:, 0], positions[:, 2] - positions[:, 0]), axis=1)
    source_areas = np.bincount(np.searchsorted(starts, boughs['index'][:, 0], side='right') - 1, weights=areas)

    # The repository carries the cutout mask. Give it a varied RGB sheet for the offline test.
    atlas = np.empty(mask.shape + (4,), np.uint8)
    atlas[..., :3] = config['palette']['needles_srgb']
    atlas[:mask.shape[0] // 2, :, :3] += 1
    atlas[:, :mask.shape[1] // 2, :3] -= 1
    atlas[..., 3] = mask * 255

    assert [settings['px'] for settings in config['levels']] == [400, 200, 100, 50]
    settings = next(level for level in config['levels'] if level['px'] == px)
    level = fir_tree.design_level(tree, settings, mask, atlas)
    level_wood, level_boughs = level.parts
    triangles = sum(len(part['index']) for part in level.parts)
    assert triangles <= int(tree.metrics['triangles'] * settings['share'])
    assert triangles <= fir_tree.thin(tree, old_share, seed=4).metrics['triangles']
    assert (level_wood['owner'] == 0).all()
    assert any(len(card['members']) > 1 for card in level.cards)
    assert sum(card['count'] for card in level.cards) == len(level_boughs['colour'])
    for card in level.cards:
        source = card['source']
        members = card['members']
        assert source in members
        assert source_areas[source] == pytest.approx(max(source_areas[members]), rel=1e-6), card
        original = tree.cards[source]
        retained = level_boughs['colour'][card['first']:card['first'] + card['count']]
        assert float(retained[:, 3].max()) / 255 >= settings['min_open'], card
        np.testing.assert_array_equal(retained, source_colour[original['first']:original['first'] + original['count']])
    assert digest(tree) == before
    for part, original in zip(tree.parts, normals):
        np.testing.assert_array_equal(part['normal'], original)

    comparison = tree_density.compare_level(tree.parts, level.parts, mask, atlas, px)
    assert len(comparison['views']) == 5
    openings = {view['view']: view['background_level'] - view['background_full'] for view in comparison['views']}
    assert max(map(abs, openings.values())) <= .01, (px, openings)
    assert abs(comparison['tone']['relative']) <= .005, (px, comparison['tone'])
