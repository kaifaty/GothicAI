"""Tests of the world census (world_census.py, world_classes.py).

Synthetic worlds of a few chunks, no game data; the last test checks the research numbers on NEWWORLD of the local
base and skips without it.
"""
import json
import os
import sys
from pathlib import Path

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import world_census as census  # noqa: E402
from test_world_edit import write_glb  # noqa: E402

GROUND = {'name': 'NW_NATURE_WALDBODEN_03', 'group': 4, 'texture': 'NW_NATURE_WALDBODEN_03.TGA'}
ROCK = {'name': 'NW_NATURE_STEIN_03', 'group': 2, 'texture': 'NW_NATURE_STEIN_03.TGA'}
MATERIALS = [GROUND, ROCK]


def square(x0, z0, size, y=0.0, slope=0.0):
    """Two up-facing triangles over Gothic x0..x0+size, z0..z0+size (metres), height y + slope * (x - x0); as glTF
    centimetres (x mirrored)."""
    corners = [(x0, z0), (x0 + size, z0), (x0 + size, z0 + size), (x0, z0 + size)]
    pos = np.array([(-x * 100, (y + slope * (x - x0)) * 100, z * 100) for x, z in corners], np.float32)
    tris = []
    for t in ((0, 1, 2), (0, 2, 3)):
        a, b, c = pos[list(t)]
        tris.append(t if np.cross(b - a, c - a)[1] > 0 else t[::-1])
    return pos, tris


def chunk(path, *squares, material=0):
    """A chunk of squares of one material."""
    pos, tris = [], []
    for p, t in squares:
        offset = sum(len(q) for q in pos)
        tris += [[i + offset for i in tri] for tri in t]
        pos.append(p)
    pos = np.concatenate(pos)
    write_glb(path, [(material, pos, np.full(len(pos), -1), tris)], MATERIALS)


def make_world(root, chunks, vobs=()):
    """A base with world TEST: chunks is {name: [squares]}, vobs a list of (id, visual, Gothic position cm)."""
    wdir = root / 'worlds' / 'TEST'
    for name, squares in chunks.items():
        chunk(wdir / 'chunks' / f'{name}.glb', *squares)
    (wdir / 'world.json').write_text(json.dumps({
        'chunks': [f'chunks/{n}.glb' for n in chunks],
        'materials': [{'index': i, **m} for i, m in enumerate(MATERIALS)]}))
    (wdir / 'vobs.json').write_text(json.dumps({'vobs': [
        {'id': i, 'visual_name': v, 'position': list(p), 'children': []} for i, v, p in vobs]}))
    tree = square(0, 0, 1)
    write_glb(root / 'meshes' / 'NW_NATURE_TANNE_75P.glb', [(0, tree[0], np.full(4, -1), tree[1])], MATERIALS)
    return root


def run(base, region=None, content=None, cell=1.0):
    return census.census(base, 'TEST', content, region, cell)


def test_flat_square_is_one_layer_with_its_density_and_plan_area(tmp_path):
    R = run(make_world(tmp_path, {'0_0': [square(0, 0, 10)]}))
    ground = R['classes']['ground']['land']
    assert R['totals']['triangles'] == 2
    assert ground['area_m2'] == pytest.approx(100)
    assert ground['tris_per_m2'] == pytest.approx(0.02)
    assert R['totals']['plan_up_m2'] == pytest.approx(100)
    assert R['sea_level_m'] is None
    layers = R['layers']['natural']
    assert (layers['cells'], layers['one'], layers['two_or_more']) == (100, 100, 0)
    assert R['totals']['other_share'] == 0
    assert run(tmp_path, cell=2.0)['layers']['natural']['cells'] == 25


@pytest.mark.parametrize('gap, layers', [(3.0, 2), (2.0, 1), (1.5, 1)])
def test_terraces_over_the_same_cells_are_two_layers_only_more_than_2_m_apart(tmp_path, gap, layers):
    R = run(make_world(tmp_path, {'0_0': [square(0, 0, 4), square(0, 0, 4, y=gap)]}))
    L = R['layers']['natural']
    assert L['cells'] == 16
    assert (L['one'], L['two_or_more']) == ((16, 0) if layers == 1 else (0, 16))


def test_a_45_degree_plane_is_in_the_45_60_band_a_band_edge_belongs_to_the_steeper_band(tmp_path):
    R = run(make_world(tmp_path, {'0_0': [square(0, 0, 10, slope=1.0)]}))
    bands = dict(zip(('0-15', '15-30', '30-45', '45-60', '60-90', 'out', 'ceiling'), R['slope']['ground']['all']))
    assert bands['45-60'] == pytest.approx(1.0)
    assert bands['30-45'] == 0 and sum(bands.values()) == pytest.approx(1.0)
    assert R['classes']['ground']['all']['area_m2'] == pytest.approx(100 * 2 ** 0.5)


def test_region_by_chunks_and_by_rect(tmp_path, capsys):
    base = make_world(tmp_path / 'base', {n: [square(64 * x, 64 * z, 8)] for n, (x, z) in
                                          {'0_0': (0, 0), '1_0': (1, 0), '0_1': (0, 1), '-1_0': (-1, 0), '5_5': (5, 5)}.items()},
                      vobs=[(1, 'NW_NATURE_TANNE_75P.3DS', (500, 0, 500)), (2, 'NW_NATURE_TANNE_75P.3DS', (33000, 0, 33000))])
    out = tmp_path / 'out'
    assert census.main(['TEST', '--base', str(base), '--chunks', '0_0,1_0', '--out', str(out)]) == 0
    R = json.loads((out / 'TEST_census.json').read_text())
    assert (R['totals']['triangles'], R['totals']['chunk_files']) == (4, 2)
    assert R['objects']['classes']['trees']['instances'] == 1
    assert (out / 'TEST_census.md').read_text() + '\n' == capsys.readouterr().out
    capsys.readouterr()
    assert census.main(['TEST', '--base', str(base), '--rect', '-1,0,0,1', '--out', str(out)]) == 0
    R = json.loads((out / 'TEST_census.json').read_text())
    assert (R['totals']['triangles'], R['totals']['chunk_files'], R['missing_chunks']) == (6, 3, ['-1_1'])
    assert census.main(['TEST', '--base', str(base), '--chunks', '-1_0', '--out', str(out)]) == 0
    assert json.loads((out / 'TEST_census.json').read_text())['totals']['triangles'] == 2
    R = run(base)
    assert (R['totals']['triangles'], R['objects']['classes']['trees']['instances']) == (10, 2)


def test_a_content_chunk_replaces_the_base_chunk(tmp_path):
    base = make_world(tmp_path / 'base', {'0_0': [square(0, 0, 8)], '1_0': [square(64, 0, 8)]})
    content = tmp_path / 'content'
    chunk(content / 'worlds' / 'TEST' / 'chunks' / '0_0.glb', square(0, 0, 4), square(4, 0, 4), material=1)
    R = run(base, content=content)
    assert R['content_chunks'] == ['0_0']
    assert R['totals']['triangles'] == 6
    assert R['classes']['rock']['all']['triangles'] == 4
    assert R['classes']['ground']['all']['triangles'] == 2
    assert R['classes']['rock']['all']['area_m2'] == pytest.approx(32)
    assert R['layers']['natural']['cells'] == 32 + 64


def test_a_vob_layer_adds_a_mesh_and_removes_an_id(tmp_path):
    base = make_world(tmp_path / 'base', {'0_0': [square(0, 0, 8)]},
                      vobs=[(1, 'NW_NATURE_TANNE_75P.3DS', (100, 0, 100)), (2, 'NW_NATURE_TANNE_75P.3DS', (200, 0, 200))])
    content = tmp_path / 'content'
    (content / 'worlds' / 'TEST' / 'vobs.d').mkdir(parents=True)
    (content / 'worlds' / 'TEST' / 'vobs.d' / 'test.json').write_text(json.dumps({
        'format': 1, 'remove': [1],
        'add': [{'key': 'bush', 'kind': 'mesh', 'visual': 'OWN_BUSH', 'position': [300, 0, 300], 'yaw_deg': 0,
                 'collision': 'mesh'}]}))
    bush = square(0, 0, 1)
    write_glb(content / 'meshes' / 'own-bush' / 'model.glb', [(0, bush[0], np.full(4, -1), bush[1])], MATERIALS)
    (content / 'meshes' / 'meshes.json').write_text(json.dumps({'OWN_BUSH': {'glb': 'own-bush/model.glb'}}))
    vobs, _ = census.load_vobs(base, 'TEST', content)
    assert [v.id for v in vobs] == [2, None]
    o = run(base, content=content)['objects']
    assert o['classes']['trees']['instances'] == 1 and o['classes']['trees']['mesh_triangles'] == 2
    assert o['classes']['bushes']['instances'] == 1 and o['classes']['bushes']['mesh_triangles'] == 2
    assert o['layers'] == [{'file': 'test.json', 'meshes': 1, 'other_added': 0, 'removed': 1, 'moved': 0,
                            'rejected': []}]
    (content / 'worlds' / 'TEST' / 'vobs.d' / 'test.json').write_text(json.dumps({
        'format': 1, 'remove': [1, 1],
        'add': [{'key': 'bush', 'kind': 'mesh', 'visual': 'OWN_BUSH', 'position': [300, 0, 300], 'yaw_deg': 0,
                 'collision': 'mesh'}]}))
    vobs, layers = census.load_vobs(base, 'TEST', content)
    assert [v.id for v in vobs] == [1, 2] and layers[0]['rejected'] == ['id 1 removed twice']


def test_the_base_is_the_newest_cache_directory_with_base_json(tmp_path, monkeypatch):
    cache = tmp_path / '.cache' / 'gothic-ai' / 'base'
    for name, mtime in (('old', 1000), ('new', 2000), ('none', 3000)):
        (cache / name).mkdir(parents=True)
        if name != 'none':
            (cache / name / 'base.json').write_text('{}')
            os.utime(cache / name / 'base.json', (mtime, mtime))
    monkeypatch.setenv('HOME', str(tmp_path))
    monkeypatch.delenv('OPENGOTHIC_BASE', raising=False)
    assert census.resolve_base(None) == cache / 'new'
    monkeypatch.setenv('OPENGOTHIC_BASE', str(tmp_path / 'env'))
    assert census.resolve_base(None) == tmp_path / 'env'
    assert census.resolve_base(str(tmp_path / 'arg')) == tmp_path / 'arg'


def test_a_visual_counts_the_same_mesh_with_no_suffix_3ds_or_mrm(tmp_path):
    names = ('NW_NATURE_TANNE_75P', 'OWN_BUSH')
    base = make_world(tmp_path / 'base', {'0_0': [square(0, 0, 8)]},
                      vobs=[(i, n + suffix, (100, 0, 100)) for i, (n, suffix) in
                            enumerate(((n, s) for n in names for s in ('', '.3DS', '.MRM')), 1)])
    content = tmp_path / 'content'
    bush = square(0, 0, 1)
    write_glb(content / 'meshes' / 'own-bush' / 'model.glb', [(0, bush[0], np.full(4, -1), bush[1])], MATERIALS)
    (content / 'meshes' / 'meshes.json').write_text(json.dumps({'OWN_BUSH': {'glb': 'own-bush/model.glb'}}))
    o = run(base, content=content)['objects']['classes']
    for cls in ('trees', 'bushes'):
        assert (o[cls]['instances'], o[cls]['instances_with_mesh'], o[cls]['mesh_triangles']) == (3, 3, 6)


def test_newworld_of_the_base_gives_the_research_numbers():
    try:
        base = census.resolve_base(None)
    except SystemExit:
        pytest.skip('no base')
    if not (base / 'worlds' / 'NEWWORLD' / 'world.json').is_file():
        pytest.skip('no NEWWORLD in the base')
    R = census.census(base, 'NEWWORLD')
    assert R['totals']['triangles'] == 473_502
    ground, rock = R['classes']['ground']['land'], R['classes']['rock']['land']
    assert (ground['triangles'], round(ground['area_m2'])) == (35_861, 438_517)
    assert (rock['triangles'], round(rock['area_m2'])) == (98_300, 1_203_961)
    natural = R['layers']['natural']
    assert (natural['cells'], natural['one']) == (771_966, 703_447)
    trees, bushes = R['objects']['classes']['trees'], R['objects']['classes']['bushes']
    assert (trees['instances'], bushes['instances']) == (1_643, 1_652)
