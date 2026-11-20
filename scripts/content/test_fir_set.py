"""The forest of the cost stand (scripts/content/fir_set.py forest, reports/engine/2026-11-16-modern-trees/tree-cost.md):
the forests of 64, 256 and 1 024 spruces nest, every place keeps its spruce and yaw, the grid is STEP apart."""
import json
from concurrent.futures import ThreadPoolExecutor
from threading import Barrier
from types import SimpleNamespace

import numpy as np
import pytest

import fir_set
import impostor_bake
from mesh3d.blender import tree_glb


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
    import sys
    sys.path.insert(0, str(fir_set.BLENDER))
    import fir_tree
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


def test_baking_the_same_mesh_level_produces_identical_atlases(tmp_path, monkeypatch):
    # Match the shader's frame grid and the engine's DDS dimensions before shrinking the fixture.
    assert (impostor_bake.FRAMES, impostor_bake.TILE, impostor_bake.PADDING, impostor_bake.MIPS) == (12, 128, 16, 5)
    monkeypatch.setattr(impostor_bake, 'FRAMES', 4)
    monkeypatch.setattr(impostor_bake, 'TILE', 16)
    monkeypatch.setattr(impostor_bake, 'PADDING', 4)
    monkeypatch.setattr(impostor_bake, 'MIPS', 3)
    meshes = tmp_path / 'meshes'
    materials = tmp_path / 'materials'
    materials.mkdir()
    part = dict(name='OWN_LEAF', position=np.array(((-1, 0, 0), (1, 0, 0), (0, 1, 2))),
                normal=np.tile((0, -1, 0), (3, 1)), uv=np.array(((0, 1), (1, 1), (.5, 0))),
                index=np.array(((0, 1, 2),)), colour=np.tile((0, 0, 0, 128), (3, 1)), cutout=True)
    tree_glb.write(meshes / 'base.glb', [part])
    tree_glb.write(meshes / 'level.glb', [part])
    dds = fir_set.sibling('test-material.py')
    image = np.tile((.2, .5, .3, 1), (4, 4, 1))
    image[::2, ::2, 3] = 0
    dds.write_dds(materials / 'leaf.dds', image)
    (materials / 'materials.json').write_text(json.dumps({'OWN_LEAF.TGA': {
        'albedo': 'leaf.dds', 'two_sided': 'same', 'foliage': {'anchor': 'tree', 'translucency': .35},
    }}))
    source = {'glb': 'level.glb', 'px': 50}
    index = meshes / 'meshes.json'
    visuals = ('OWN_LEAF', 'OWN_ALIAS')
    index.write_text(json.dumps({visual: {'glb': 'base.glb', 'levels': [source]} for visual in visuals}))

    start = Barrier(2)

    def bake_visual(visual):
        start.wait(timeout=10)
        return impostor_bake.bake(tmp_path, visual, 49)

    with ThreadPoolExecutor(max_workers=2) as pool:
        first, alias = pool.map(bake_visual, visuals)
    assert first['atlases'] == alias['atlases']
    entry = {'impostor': first['atlases'], 'px': 49}
    assert [record['levels'] for record in json.loads(index.read_text()).values()] == [[source, entry]] * 2
    paths = {layer: meshes / path for layer, path in first['atlases'].items()}
    atlases = {layer: path.read_bytes() for layer, path in paths.items()}
    coverage = impostor_bake.dds_mips(paths['albedo'])[0][..., 3]
    assert 0 < np.count_nonzero(coverage) < coverage.size
    for path in paths.values():
        path.unlink()
    assert impostor_bake.bake(tmp_path, 'OWN_LEAF', 49) == first
    assert {layer: path.read_bytes() for layer, path in paths.items()} == atlases

    part['position'][2, 2] = 1
    tree_glb.write(meshes / 'level.glb', [part])
    updated = impostor_bake.bake(tmp_path, 'OWN_LEAF', 49)
    assert updated['atlases'] != first['atlases']
    assert all(path.exists() for path in paths.values())
    assert impostor_bake.bake(tmp_path, 'OWN_ALIAS', 49)['atlases'] == updated['atlases']
    assert set(meshes.glob('*.impostor-*.dds')) == {meshes / path for path in updated['atlases'].values()}
    entry = {'impostor': updated['atlases'], 'px': 49}
    assert [record['levels'] for record in json.loads(index.read_text()).values()] == [[source, entry]] * 2


def test_bough_x4_adds_hierarchy_without_changing_the_spruce_or_its_fallbacks(tmp_path, monkeypatch):
    control, impostor_root, hierarchy_root = (tmp_path / name for name in ('control', 'impostor', 'bake'))
    probes = tmp_path / 'probes'
    monkeypatch.setattr(fir_set, 'PROBES', probes)
    config = fir_set.BLENDER / 'fir.json'

    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else json.dumps(data).encode())

    base = 'OWN_TREE_FIR_A_4'
    visual = 'OWN_TREE_FIR_A_BOUGH_X4'
    control_visual = 'OWN_TREE_FIR_A_BOUGH_CONTROL'
    full = {'glb': 'trees_fir/base.glb', 'levels': [
        {'glb': f'trees_fir/L{i}.glb', 'px': px} for i, px in enumerate((400, 200, 100, 50), 1)]}
    mesh_bytes = {record['glb']: record['glb'].encode() for record in (full, *full['levels'])}
    for path, data in mesh_bytes.items():
        write(control / 'meshes' / path, data)
    write(control / 'meshes/meshes.json', {base: full})
    boughs = {'albedo': 'trees_fir/boughs.dds', 'normal': 'trees_fir/boughs-normal.dds',
              'orm': 'trees_fir/boughs-orm.dds', 'foliage': fir_set.FOLIAGE['boughs'], 'two_sided': 'same'}
    boughs_key = 'OWN_TREE_FIR_BOUGHS.TGA'
    write(control / 'materials/materials.json', {boughs_key: boughs})
    for kind in ('albedo', 'normal', 'orm'):
        write(control / 'materials' / boughs[kind], boughs[kind].encode())
    layer = 'worlds/NEWWORLD/vobs.d'
    write(control / layer / 'fir-probe.json', {})

    impostor = {'impostor': {kind: f'trees_fir/impostor-{kind}.dds' for kind in ('albedo', 'surface', 'orm')},
                'px': 49}
    write(impostor_root / 'meshes/meshes.json', {base: {'levels': [impostor]}})
    for path in impostor['impostor'].values():
        write(impostor_root / 'meshes' / path, path.encode())
    hierarchy = {'glb': f'{visual}_H.glb', 'atlas': {'pages': [{'rgba': 'color.dds', 'attributes': 'normal.dds'}]},
                 'nodes': [{'geometry': {'primitive': 1}}, {'geometry': {'primitive': 2, 'page': 0}}]}
    sidecar = f'{visual}_H.json'
    write(hierarchy_root / sidecar, hierarchy)
    for path in ('color.dds', 'normal.dds', hierarchy['glb']):
        write(hierarchy_root / path, path.encode())

    fir_set.bough_x4(SimpleNamespace(config=config, control=control, impostor_from=impostor_root,
                                    hierarchy=hierarchy_root, set='bough-x4/content'))
    content = probes / 'bough-x4/content'
    meshes = json.loads((content / 'meshes/meshes.json').read_text())
    staged_control = dict(full, levels=[*full['levels'], impostor])
    assert meshes == {base: full, control_visual: staged_control,
                      visual: dict(staged_control, hierarchy=f'trees_fir/bough-x4/{sidecar}')}
    assert json.loads((control / 'meshes/meshes.json').read_text()) == {base: full}
    for path, data in mesh_bytes.items():
        assert (content / 'meshes' / path).read_bytes() == (control / 'meshes' / path).read_bytes() == data
    for path in impostor['impostor'].values():
        assert (content / 'meshes' / path).read_bytes() == path.encode()

    staged_sidecar = content / 'meshes' / meshes[visual]['hierarchy']
    assert staged_sidecar.read_bytes() == (hierarchy_root / sidecar).read_bytes()
    assert (staged_sidecar.parent / hierarchy['glb']).read_bytes() == hierarchy['glb'].encode()
    materials = json.loads((content / 'materials/materials.json').read_text())
    assert materials[boughs_key] == boughs
    for kind in ('albedo', 'normal', 'orm'):
        assert (content / 'materials' / boughs[kind]).read_bytes() == boughs[kind].encode()
    page = materials['FIR_HIERARCHY_0_2.TGA']
    assert page == {'albedo': 'trees_fir/bough-x4/color.dds', 'normal': 'trees_fir/bough-x4/normal.dds',
                    'foliage': boughs['foliage'], 'two_sided': boughs['two_sided']}
    for path in (page['albedo'], page['normal']):
        staged = content / 'materials' / path
        assert staged.read_bytes() == (staged_sidecar.parent / staged.name).read_bytes() == staged.name.encode()
    assert not (content / layer / 'fir-probe.json').exists()
    placements = json.loads((content / layer / 'fir-bough-x4.json').read_text())['add']
    assert [(item['visual'], tuple(item['position'])) for item in placements] == list(zip(
        (control_visual, visual), fir_set.EYE_ROW[:2], strict=True))

    grid = fir_set.forest_layout(1024)
    forest = [dict(key=f'tree-cost-{t["column"]}-{t["row"]}', visual=base,
                   position=[t['x'], 0, t['z']], yaw_deg=t['yaw'], collision='none') for t in grid]
    write(control / layer / 'tree-cost.json', {'format': 1, 'add': forest})
    for path in impostor['impostor'].values():
        write(control / 'meshes' / path, path.encode())
    write(control / 'meshes/meshes.json', {base: staged_control})
    old_level = control / 'meshes' / full['levels'][0]['glb']
    old_level.unlink()
    write(old_level, b'older forest level')
    world = 'worlds/NEWWORLD/vobs.json'
    base_world = {'format': 1, 'source': {}, 'vobs': [{'object_id': 9, 'id': 20, 'children': []}]}
    write(tmp_path / world, base_world)
    monkeypatch.setattr(tree_glb, 'read', lambda _: [{'position': np.array([[-1, -1, 0], [1, 1, 2]])}])
    fir_set.forest(SimpleNamespace(config=config, source=control, hierarchy=content, base=tmp_path,
                                   set='bough-x4/forest-1024', count=1024, share=1, levels=None, textures=None))
    staged = probes / 'bough-x4/forest-1024'
    assert not (staged / layer / 'tree-cost.json').exists()
    assert (control / layer / 'tree-cost.json').exists()
    assert json.loads((staged / 'meshes/meshes.json').read_text()) == {
        base: dict(staged_control, hierarchy=f'trees_fir/bough-x4/{sidecar}')}
    assert json.loads((control / 'meshes/meshes.json').read_text()) == {base: staged_control}
    assert old_level.read_bytes() == b'older forest level'
    for path, data in mesh_bytes.items():
        assert (staged / 'meshes' / path).read_bytes() == data
    records = json.loads((staged / world).read_text())['vobs']
    assert records[0] == base_world['vobs'][0]
    assert len(records) == 1025
    assert records[1]['visual']['name'] == f'{base}.3DS'
    assert all(t['visual'] == {'ref': records[1]['visual']['object_id']} for t in records[2:])
    assert [t['position'] for t in records[1:]] == [t['position'] for t in forest]
    for record, place in zip(records[1:], grid, strict=True):
        scale = fir_set.PROBE[place['spruce'] - 1][2]
        yaw = np.radians(place['yaw'])
        assert np.array(record['rotation'])[:, 0] == pytest.approx(scale * np.array([np.cos(yaw), 0, -np.sin(yaw)]))
        assert record['rotation'][1][1] == scale


def test_spruce_a_and_its_four_levels_rebuild_byte_for_byte(tmp_path, monkeypatch):
    reference = fir_set.PROBES / 'lod-designed/fir-probe-4'
    textures = reference / 'materials/trees_fir'
    meshes = reference / 'meshes/trees_fir'
    visual = 'OWN_TREE_FIR_A_4'
    names = [f'{visual}.glb', *(f'{visual}_L{i}.glb' for i in range(1, 5))]
    if not (textures / 'boughs-albedo.dds').is_file() or not all((meshes / name).is_file() for name in names):
        pytest.skip('Requires the accepted fir-probe-4 GLBs and canopy atlas')

    monkeypatch.syspath_prepend(str(fir_set.BLENDER))
    import fir_tree
    import tree_density

    config = json.loads((fir_set.BLENDER / 'fir.json').read_text())
    tree = fir_tree.build(config)
    extras = dict(tree=config['id'], seed=7, scale=1.)
    tree_glb.write(tmp_path / names[0], tree.parts, visual, extras=extras)
    mask = tree_density.load_mask(fir_set.BLENDER / config['atlas']['mask'])
    atlas = fir_set.load_level_atlas(textures, 'designed')
    fir_set.write_levels(tmp_path, visual, tree, 4, extras, config, mask, atlas)
    for name in names:
        assert (tmp_path / name).read_bytes() == (meshes / name).read_bytes(), name


def test_tree_occlusion_probe_detects_clear_depth_outside_the_projected_box():
    import runpy

    helper = fir_set.HERE.parents[1] / 'reports/engine/2026-11-16-modern-trees/tree-hidden.py'
    Depth = runpy.run_path(str(helper))['Depth']
    depth = np.full((32, 32), 0.97, dtype=np.float32)
    depth[4, 4] = 1
    depth[8, 16] = 1
    pyramid = Depth(depth, (8, 8))

    single_tile = np.array([[1, 1], [3, 3]], dtype=np.float32) / 32
    assert pyramid.test(single_tile, 0.98) == (False, 0)

    projected_box = np.array([[1, 1], [4, 1]], dtype=np.float32) / 32
    assert pyramid.test(projected_box, 0.98) == (True, 0)
    assert pyramid.test(projected_box, 0.98, 'neighbors') == (False, 0)

    three_tiles = np.array([[17, 1], [24, 1]], dtype=np.float32) / 32
    assert pyramid.test(three_tiles, 0.98) == (True, 1)
    assert pyramid.test(three_tiles, 0.98, 'tight_mip') == (False, 1)

    # At the bottom of pack_depth's range, the HiZ bias adds two R16 steps.
    assert Depth.encode(np.float32(0.95)) == 2
    clear = Depth(np.ones((1, 1), dtype=np.float32), (1, 1))
    assert clear.test(np.array([[0, 0], [1, 1]], dtype=np.float32), 1, 'precise') == (True, 0)
