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


@pytest.mark.parametrize('method', ['designed', 'random'])
def test_the_levels_are_the_field_the_engine_takes_and_keep_the_spruces_materials(tmp_path, monkeypatch, method):
    """Each mode writes four GLBs with the materials and COLOR_0 required by the engine."""
    import json
    import sys
    sys.path.insert(0, str(fir_set.BLENDER))
    import fir_tree
    import tree_glb
    config = json.loads((fir_set.BLENDER / 'fir.json').read_text())
    full = fir_tree.build(config)
    mask = np.ones((2, 2), dtype=bool)
    atlas = np.zeros((2, 2, 4), dtype=np.uint8)
    designed = []

    def design_level(tree, level, given_mask, given_atlas):
        assert tree is full and given_mask is mask and given_atlas is atlas
        designed.append(level)
        return fir_tree.thin(tree, level['share'], 4)

    monkeypatch.setattr(fir_tree, 'design_level', design_level)
    options = {'method': 'random'} if method == 'random' else {}
    field, numbers = fir_set.write_levels(tmp_path, 'OWN_TREE_FIR_A_4', full, 4, dict(tree=config['id']),
                                          config, mask, atlas, **options)
    assert [f['px'] for f in field] == [400, 200, 100, 50]
    assert [f['glb'] for f in field] == [f'{fir_set.FOLDER}/OWN_TREE_FIR_A_4_L{k}.glb' for k in (1, 2, 3, 4)]
    if method == 'random':
        assert [level['share'] for level in numbers] == [.5, .25, .125, 1 / 32]
        assert numbers[-1]['triangles'] / full.metrics['triangles'] == pytest.approx(1 / 32, rel=.02)
        assert not designed
    else:
        assert designed == config['levels']
        assert [level['share'] for level in numbers] == [level['share'] for level in config['levels']]
    names = [p['name'] for p in full.parts]
    for level in numbers:
        prims = tree_glb.read(tmp_path / f"OWN_TREE_FIR_A_4_L{level['level']}.glb")
        assert sorted(p['name'] for p in prims) == sorted(set(names))
        assert all(p['colour_format'] == (tree_glb.UBYTE, 'VEC4', True) for p in prims)
        assert abs(level['triangles'] / full.metrics['triangles'] - level['share']) < .01
