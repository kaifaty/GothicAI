"""Tests of the world edit tool (world_edit.py, chunk_glb.py, world_bpy.py).

The check runs on synthetic chunks without Blender. The Blender tests build a synthetic world of two chunks and
apply the test edits of test_edits/; they skip without Blender. The publication tests cook with a stand-in for
content-convert (world_fixtures.fake_converter); one publishes an edit of NEWWORLD of the base with content-convert
and checks it with verify-cooked-world, and skips without them. The rebuild tests compare the records of
data/content/world-edits.jsonl with the chunks published on this machine; they skip without the base or the sets.
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import chunk_glb  # noqa: E402
import world_cook  # noqa: E402
import world_edit  # noqa: E402
from world_fixtures import (  # noqa: E402
    MATERIALS,
    blender,
    fake_converter,
    fake_wcb,
    follow_writes,
    write_glb,
)

EDITS = HERE / 'test_edits'


def grid(i0, i1, j0=0, j1=2):
    """Quads over glTF x = 100 i, z = 100 j; vertex id 3 i + j; each quad two triangles."""
    verts = [(i, j) for i in range(i0, i1 + 1) for j in range(j0, j1 + 1)]
    index = {v: k for k, v in enumerate(verts)}
    tris = []
    for i in range(i0, i1):
        for j in range(j0, j1):
            a, b, c, d = index[(i, j)], index[(i + 1, j)], index[(i + 1, j + 1)], index[(i, j + 1)]
            tris += [(a, c, b), (a, d, c)]
    return [(100.0 * i, 0.0, 100.0 * j) for i, j in verts], [3 * i + j for i, j in verts], tris


def world(tmp_path):
    """A base with two chunks side by side: 0_0 is the grid i 0..2, 1_0 the grid i 2..3 (shares i = 2)."""
    base = tmp_path / 'base'
    chunks = base / 'worlds/TESTWORLD/chunks'
    pos, ids, tris = grid(0, 2)
    write_glb(chunks / '0_0.glb', [(0, pos, ids, tris)])
    pos, ids, tris = grid(2, 3)
    write_glb(chunks / '1_0.glb', [(1, pos, ids, tris)])
    (base / 'worlds/TESTWORLD/world.json').write_text(json.dumps({'materials': MATERIALS}))
    return base


def edited(tmp_path, base, change):
    pos, ids, tris = grid(0, 2)
    pos, ids, tris, prims = change(list(pos), list(ids), list(tris))
    out = tmp_path / 'edited.glb'
    write_glb(out, prims or [(0, pos, ids, tris)],
              MATERIALS + [{'name': 'TEST_UNKNOWN', 'texture': 'TEST_UNKNOWN.TGA'}])
    return chunk_glb.check(base / 'worlds/TESTWORLD/chunks/0_0.glb', out)


def test_check_passes_an_unchanged_chunk(tmp_path):
    base = world(tmp_path)
    report = edited(tmp_path, base, lambda p, i, t: (p, i, t, None))
    assert report.ok, report.problems
    assert report.summary['border_vertices'] == 3
    assert report.summary['triangles'] == [8, 8]


def test_check_catches_a_moved_border_vertex(tmp_path):
    base = world(tmp_path)

    def move(p, i, t):
        p[i.index(7)] = (200.0, 10.0, 100.0)
        return p, i, t, None
    report = edited(tmp_path, base, move)
    assert any('border vertex 7 moved' in s for s in report.problems)
    assert any('border vertex 7' in s and 'gone' in s for s in report.problems)


def test_check_catches_a_new_open_edge(tmp_path):
    base = world(tmp_path)
    report = edited(tmp_path, base, lambda p, i, t: (p, i, t[1:], None))
    assert any(s.startswith('new open edge') for s in report.problems)
    assert report.summary['materials']['TEST_A']['triangles'] == [8, 7]


def test_check_catches_an_unknown_material(tmp_path):
    base = world(tmp_path)
    report = edited(tmp_path, base, lambda p, i, t: (p, i, t, [(0, p, i, t[1:]), (2, p, i, t[:1])]))
    assert any("unknown material 'TEST_UNKNOWN'" in s for s in report.problems)
    index = tmp_path / 'materials.json'
    index.write_text(json.dumps({'TEST_UNKNOWN.TGA': {}}))
    out = tmp_path / 'edited.glb'
    known = chunk_glb.check(base / 'worlds/TESTWORLD/chunks/0_0.glb', out, material_indexes=[index])
    assert known.ok, known.problems


def test_check_reports_area_per_material(tmp_path):
    base = world(tmp_path)

    def lift(p, i, t):
        p[i.index(4)] = (100.0, 100.0, 100.0)    # the middle vertex: not on the border
        return p, i, t, None
    report = edited(tmp_path, base, lift)
    assert report.ok, report.problems
    row = report.summary['materials']['TEST_A']
    assert row['triangles'] == [8, 8] and row['area_m2'][1] > row['area_m2'][0] == 4.0


def test_same_geometry_ignores_order(tmp_path):
    pos, ids, tris = grid(0, 2)
    write_glb(tmp_path / 'a.glb', [(0, pos, ids, tris)])
    write_glb(tmp_path / 'b.glb', [(0, pos, ids, list(reversed(tris)))])
    assert chunk_glb.same_geometry(tmp_path / 'a.glb', tmp_path / 'b.glb', full=True) == []
    write_glb(tmp_path / 'c.glb', [(1, pos, ids, tris)])
    assert chunk_glb.same_geometry(tmp_path / 'a.glb', tmp_path / 'c.glb')


def test_check_catches_a_changed_vertex_light(tmp_path):
    # two materials with their own light; the export that dropped COLOR_0 turned the second one white
    pos, ids, tris = grid(0, 2)
    half = len(tris) // 2
    light = [0.2 + 0.05 * k for k in range(len(pos))]
    base = tmp_path / 'base/0_0.glb'
    write_glb(base, [(0, pos, ids, tris[:half], light), (1, pos, ids, tris[half:], light)])
    write_glb(tmp_path / 'same.glb', [(0, pos, ids, tris[:half], light), (1, pos, ids, tris[half:], light)])
    same = chunk_glb.check(base, tmp_path / 'same.glb', world_json=tmp_path / 'none.json')
    assert not any('vertex light' in p for p in same.problems)
    assert same.summary['light']['checked'] == 3 * len(tris) and same.summary['light']['other'] == 0
    write_glb(tmp_path / 'white.glb', [(0, pos, ids, tris[:half], light), (1, pos, ids, tris[half:], [1.0] * len(pos))])
    white = chunk_glb.check(base, tmp_path / 'white.glb', world_json=tmp_path / 'none.json')
    assert any(p.startswith('vertex light') and '(TEST_B)' in p for p in white.problems)
    assert white.summary['light']['other'] == 3 * (len(tris) - half)


def source_chunk(path, tris_per_material=4):
    """A chunk as the converter writes it, with order, flags and lightmap per triangle: two materials, each a strip
    of triangles over its own corners. Returns the corners and the primitives."""
    import chunk_canon
    corners, prims = [], []
    for mi in range(2):
        rows = []
        for t in range(tris_per_material):
            tri = []
            for k in range(3):
                c = np.zeros((), chunk_canon.CORNER)
                n = len(corners)
                c['pos'] = (100.0 * (t + k), 10.0 * mi, 100.0 * (k % 2))
                c['nrm'] = np.array([0.1 * k, 0.9, 0.3]) / np.linalg.norm([0.1 * k, 0.9, 0.3])
                c['uv'] = (0.1 * n, 0.2 * k)
                c['col'] = (50 + n, 60, 70, 255)
                c['vid'], c['fid'] = n, n
                corners.append(c)
                tri.append(n)
            rows.append(tri)
        order = np.arange(len(prims) * 100, len(prims) * 100 + len(rows), dtype=np.float32)
        flags = np.array([[3, 7 + t] for t in range(len(rows))], np.uint16)
        light = np.full(len(rows), -1 - mi, np.float32)
        prims.append((mi, rows, order, flags, light))
    doc = {'asset': {'version': '2.0', 'extras': {'gothic': {'format': 'gothic-world-chunk'}}},
           'nodes': [{'name': path.stem, 'mesh': 0, 'scale': [.01, .01, .01]}],
           'materials': [{'name': m['name'], 'extras': {'gothic': {'index': i, **m}}} for i, m in enumerate(MATERIALS)]}
    path.parent.mkdir(parents=True, exist_ok=True)
    chunk_canon._write(path, doc, doc, np.array(corners, chunk_canon.CORNER), prims)
    return np.array(corners, chunk_canon.CORNER), prims


def test_canonical_writes_source_triangles_back_exactly(tmp_path):
    # an export as Blender's: normals and light a hair off, corners rotated, the extras gone; one triangle deleted,
    # one changed past the round trip, one new
    import chunk_canon
    base = tmp_path / 'base/0_0.glb'
    corners, prims = source_chunk(base)
    off = corners.copy()
    off['nrm'] += np.float32(2e-5)
    off['col'][:, 0] += 1
    rows = []
    for mi, tris, *_ in prims:
        for t, tri in enumerate(tris):
            if (mi, t) == (0, 0):
                continue                                   # deleted
            rows.append((mi, [tri[1], tri[2], tri[0]]))     # rotated
    off['uv'][prims[1][1][3][0]] += 0.5                    # changed: its UV moved
    new = np.zeros(3, chunk_canon.CORNER)
    new['pos'] = [(0, 50, 0), (100, 50, 0), (0, 50, 100)]
    new['nrm'] = (0, 1, 0)
    new['vid'] = new['fid'] = -1
    allc = np.concatenate([off, new])
    rows.append((1, [len(off), len(off) + 1, len(off) + 2]))
    exported = [(mi, [r for m, r in rows if m == mi], None, None, None) for mi in range(2)]
    doc = {'asset': {'version': '2.0'}, 'nodes': [{'name': '0_0', 'mesh': 0, 'scale': [.01, .01, .01]}],
           'materials': [{'name': m['name'], 'extras': {'gothic': {'index': i, **m}}} for i, m in enumerate(MATERIALS)]}
    edited = tmp_path / 'edited.glb'
    chunk_canon._write(edited, doc, doc, allc, exported)
    exact, bad = chunk_canon.source_violations(base, edited)
    assert exact == 0 and any('lost its order' in b for b in bad)

    out = tmp_path / 'out.glb'
    counts = chunk_canon.canonical(base, edited, out)
    assert counts == {'source': 8, 'kept': 6, 'changed': 2}
    exact, bad = chunk_canon.source_violations(base, out)
    assert (exact, bad) == (6, [])
    _, written = chunk_canon._primitives(out)
    kept = [p for p in written if p[2] is not None]
    assert [list(p[2]) for p in kept] == [[1, 2, 3], [100, 101, 102]]
    assert all(p[3].dtype == np.uint16 and p[4] is not None for p in kept)
    _, source = chunk_canon._primitives(base)
    assert kept[0][1].tobytes() == source[0][1][1:].tobytes()
    assert sum(len(p[1]) for p in written if p[2] is None) == 2
    report = chunk_glb.check(base, out, world_json=tmp_path / 'none.json')
    assert report.summary['source'] == {'exact': 6, 'other': 0}


# --- publication of the cooked files (world_cook.py)

def cooked_world(tmp_path, objects):
    """world(tmp_path) with its chunks in world.json, as strings or as objects with a list of meshes."""
    base = world(tmp_path)
    names = [('0_0', (0, 0)), ('1_0', (1, 0))]
    chunks = [{'cell': list(xz), 'meshes': [{'file': f'chunks/{n}.glb', 'role': 'draw', 'level': 0},
                                             {'file': f'chunks/{n}.lod1.glb', 'role': 'draw', 'level': 1},
                                             {'file': f'chunks/{n}.collision.glb', 'role': 'collision'}]}
              if objects else f'chunks/{n}.glb' for n, xz in names]
    (base / 'worlds/TESTWORLD/world.json').write_text(json.dumps({'chunks': chunks, 'materials': MATERIALS}))
    return base / 'worlds/TESTWORLD', tmp_path / 'staging/test-1/worlds/TESTWORLD'


def edited_chunk(tmp_path, lift):
    pos, ids, tris = grid(0, 2)
    pos[4] = (100.0, lift, 100.0)
    out = tmp_path / f'edited-{lift}.glb'
    write_glb(out, [(0, pos, ids, tris)])
    return out


@pytest.mark.parametrize('objects', [False, True])
def test_publication_removes_the_index_first_and_writes_it_last(tmp_path, monkeypatch, capsys, objects):
    base_world, set_world = cooked_world(tmp_path, objects)
    convert = fake_converter(tmp_path / 'bin')
    for name, data in (('chunks.wci', b'old index'), ('cloth.wcl', b'old cloth'), ('chunks/0_0.wcb', b'old'),
                       ('chunks/1_0.wcb', b'no GLB beside it')):
        (set_world / name).parent.mkdir(parents=True, exist_ok=True)
        (set_world / name).write_bytes(data)
    ops = follow_writes(monkeypatch)
    publication = world_cook.Publication(base_world, set_world, convert, 'cook it')
    target = set_world / 'chunks/0_0.glb'
    publication.write(edited_chunk(tmp_path, 5.0), target)
    assert publication.finish() == 0, capsys.readouterr()
    assert ops[0] == ('remove', set_world / 'chunks.wci') and ops[1] == ('write', target)
    assert ops[-1] == ('write', set_world / 'chunks.wci')
    assert set(ops[2:-1]) == {('write', set_world / 'chunks/0_0.wcb'), ('remove', set_world / 'chunks/1_0.wcb'),
                              ('write', set_world / 'cloth.wcl')}
    index = (set_world / 'chunks.wci').read_bytes()
    assert (set_world / 'chunks/0_0.wcb').read_bytes() == fake_wcb(target)
    assert index[3:] == hashlib.sha256(fake_wcb(target) + fake_wcb(base_world / 'chunks/1_0.glb')).digest()
    assert sorted(p.relative_to(set_world).as_posix() for p in set_world.rglob('*')) == \
        ['chunks', 'chunks.wci', 'chunks/0_0.glb', 'chunks/0_0.wcb', 'cloth.wcl']
    # publishing the same again changes nothing, the index stays
    ops.clear()
    again = world_cook.Publication(base_world, set_world, convert, 'cook it')
    assert again.finish() == 0 and ops == []
    assert '.wcb 0 written, 1 left as they were; cloth bundle left; index left' in capsys.readouterr().out
    # a .wcb of the set that is not the cook's: the index goes before it is rewritten and comes back last
    (set_world / 'chunks/0_0.wcb').write_bytes(b'changed by hand')
    assert world_cook.Publication(base_world, set_world, convert, 'cook it').finish() == 0
    assert ops == [('remove', set_world / 'chunks.wci'), ('write', set_world / 'chunks/0_0.wcb'),
                   ('write', set_world / 'chunks.wci')]
    assert (set_world / 'chunks/0_0.wcb').read_bytes() == fake_wcb(target)


def test_without_a_converter_the_glb_is_published_and_no_stale_index_is_left(tmp_path, monkeypatch, capsys):
    base_world, set_world = cooked_world(tmp_path, False)
    target = set_world / 'chunks/0_0.glb'
    first = world_cook.Publication(base_world, set_world, fake_converter(tmp_path / 'bin'), 'cook it')
    first.write(edited_chunk(tmp_path, 5.0), target)
    assert first.finish() == 0 and (set_world / 'chunks.wci').exists()
    ops = follow_writes(monkeypatch)
    publication = world_cook.Publication(base_world, set_world, None, 'python3 world_edit.py cook test-1/TESTWORLD')
    publication.write(edited_chunk(tmp_path, 7.0), target)
    assert publication.finish() == 0
    assert ops[:2] == [('remove', set_world / 'chunks.wci'), ('write', target)]
    assert sorted(p.relative_to(set_world).as_posix() for p in set_world.rglob('*')) == ['chunks', 'chunks/0_0.glb']
    out = capsys.readouterr().out.splitlines()
    assert out[-1].endswith('not cooked, the game reads its GLBs (no content-convert: pass --converter or set '
                            'CONTENT_CONVERT); cook it with: python3 world_edit.py cook test-1/TESTWORLD '
                            '--converter <content-convert>')
    # cook <set>/<WORLD> cooks it later
    code = world_edit.main(['cook', 'test-1/TESTWORLD', '--base', str(base_world.parent.parent),
                            '--staging', str(tmp_path / 'staging'), '--converter', str(tmp_path / 'bin/content-convert')])
    assert code == 0 and (set_world / 'chunks/0_0.wcb').read_bytes() == fake_wcb(target)
    # a converter that fails leaves the set without an index
    monkeypatch.setenv('FAKE_CONVERTER_FAIL', 'broken')
    failed = world_cook.Publication(base_world, set_world, tmp_path / 'bin/content-convert', 'cook it')
    failed.write(edited_chunk(tmp_path, 5.0), target)
    assert failed.finish() == 1 and not (set_world / 'chunks.wci').exists()
    assert 'not cooked, the game reads its GLBs: cook-world exited 1: content-convert: broken' in capsys.readouterr().out


def add_triangle(src, dst):
    """A real base chunk with one new triangle, as an edit adds one: its corners copy the first triangle's, 1 m up."""
    import chunk_canon
    doc, prims = chunk_canon._primitives(src)
    names = [m['name'] for m in doc['materials']]
    corners, rows = [], []
    for m, tris, order, flags, light in prims:
        rows.append((names.index(m['name']), [[len(corners) + 3 * t + k for k in range(3)] for t in range(len(tris))],
                     order, flags, light))
        corners += list(tris.reshape(-1))
    new = prims[0][1][0].copy()
    new['pos'][:, 1] += 100
    new['vid'] = new['fid'] = -1
    rows.append((rows[0][0], [[len(corners), len(corners) + 1, len(corners) + 2]], None, None, None))
    corners += list(new)
    dst.parent.mkdir(parents=True, exist_ok=True)
    chunk_canon._write(dst, doc, doc, np.array(corners, chunk_canon.CORNER), rows)


def test_a_published_edit_of_the_base_passes_verify_cooked_world(tmp_path, capsys):
    convert = world_cook.converter(None)
    if convert is None:
        pytest.skip('no content-convert: set CONTENT_CONVERT')
    try:
        base = world_edit.resolve_base(None)
    except world_edit.EditError:
        pytest.skip('no base')
    source = base / 'worlds/NEWWORLD'
    if not (source / 'world.json').is_file():
        pytest.skip('no NEWWORLD in the base')
    # a scratch copy of the world: its files as links, the chunks a folder of its own for the base's .wcb
    world_dir = tmp_path / 'base/worlds/NEWWORLD'
    for path in source.rglob('*'):
        if path.is_file() and path.suffix not in ('.wcb', '.wci', '.wcl'):
            (world_dir / path.relative_to(source)).parent.mkdir(parents=True, exist_ok=True)
            (world_dir / path.relative_to(source)).symlink_to(path)

    def run(*args):
        result = subprocess.run([str(convert), *map(str, args)], capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stderr
    run('cook-world', world_dir)        # the base's own cook, beside its GLBs
    set_world = tmp_path / 'staging/test-1/worlds/NEWWORLD'
    edited = tmp_path / 'edited.glb'
    add_triangle(world_dir / 'chunks/1_-1.glb', edited)
    publication = world_cook.Publication(world_dir, set_world, convert, 'cook it')
    publication.write(edited, set_world / 'chunks/1_-1.glb')
    assert publication.finish() == 0, capsys.readouterr()
    assert sorted(p.name for p in set_world.rglob('*') if p.is_file()) == ['1_-1.glb', '1_-1.wcb', 'chunks.wci',
                                                                            'cloth.wcl']
    run('verify-cooked-world', world_dir, '--content', set_world)
    # a cook of the set in one folder: its unedited cells are the base's own, its edited cell the published one
    run('cook-world', world_dir, '--content', set_world, '--out', tmp_path / 'all')
    for wcb in (tmp_path / 'all/chunks').glob('*.wcb'):
        own = set_world / 'chunks' / wcb.name if wcb.name == '1_-1.wcb' else world_dir / 'chunks' / wcb.name
        assert wcb.read_bytes() == own.read_bytes(), wcb.name
    assert (tmp_path / 'all/chunks/1_-1.wcb').read_bytes() != (world_dir / 'chunks/1_-1.wcb').read_bytes()


# --- Blender

def build(tmp_path, base, *args, record_id='test-chunk'):
    staging, registry = tmp_path / 'staging', tmp_path / 'world-edits.jsonl'
    argv = ['build', record_id, '--base', str(base), '--staging', str(staging), '--registry', str(registry),
            '--work', str(tmp_path / 'work'), '--converter', '', *args]
    return world_edit.main(argv), staging, registry


def edit_args(*names):
    return [x for n in names for x in ('--edit', str(EDITS / f'{n}.py'))]


@blender
@pytest.mark.parametrize('name, reason', [('move_border', 'border vertex'), ('delete_face', 'new open edge'),
                                          ('strip_with_gap', 'new open edge'), ('unknown_material', 'unknown material')])
def test_build_refuses(tmp_path, capsys, name, reason):
    base = world(tmp_path)
    code, staging, registry = build(tmp_path, base, '--set', 'test-1', '--chunk', 'TESTWORLD/0_0', *edit_args(name))
    out = capsys.readouterr().out
    assert code == 1
    assert f'FAIL: {reason}' in out
    assert not registry.exists() and not (staging / 'test-1').exists()


@blender
def test_build_adds_edits_up_and_records_them(tmp_path, capsys):
    base = world(tmp_path)
    code, staging, registry = build(tmp_path, base, '--set', 'test-1', '--chunk', 'TESTWORLD/0_0',
                                    *edit_args('raise_middle'))
    assert code == 0, capsys.readouterr()
    # a second edit of another step goes on the same base chunk after the first
    code, staging, registry = build(tmp_path, base, *edit_args('add_post'))
    assert code == 0, capsys.readouterr()
    entry = json.loads(registry.read_text())
    assert [e['script'] for e in entry['edits']] == ['scripts/content/world/test_edits/raise_middle.py',
                                                     'scripts/content/world/test_edits/add_post.py']
    assert entry['edits'][0]['parameters'] == {'lift_cm': 5}
    assert entry['edits'][1]['open_edges_declared'] == 3
    assert entry['inputs'][0]['sha256'] == world_edit.sha256(base / 'worlds/TESTWORLD/chunks/0_0.glb')
    out = staging / 'test-1/worlds/TESTWORLD/chunks/0_0.glb'
    assert entry['files']['glb'] == {'path': 'test-1/worlds/TESTWORLD/chunks/0_0.glb', 'sha256': world_edit.sha256(out)}
    chunk = chunk_glb.read_chunk(out)
    positions, _ = chunk.corners()
    assert (np.abs(positions - [100, 5, 100]).max(1) < 1e-4).any()       # raise_middle
    assert chunk.triangles == 9                                            # add_post
    # a rebuild of the same list leaves the published chunk as it is
    before = out.stat().st_mtime_ns
    code, _, _ = build(tmp_path, base)
    assert code == 0 and out.stat().st_mtime_ns == before
    assert 'left as it is' in capsys.readouterr().out


@blender
def test_build_publishes_the_cooked_files_index_first_and_last(tmp_path, monkeypatch, capsys):
    base_world, set_world = cooked_world(tmp_path, False)
    for name in ('chunks.wci', 'chunks/0_0.wcb'):
        (set_world / name).parent.mkdir(parents=True, exist_ok=True)
        (set_world / name).write_bytes(b'old')
    ops = follow_writes(monkeypatch)
    code, _, _ = build(tmp_path, base_world.parent.parent, '--set', 'test-1', '--chunk', 'TESTWORLD/0_0',
                       *edit_args('raise_middle'), '--converter', str(fake_converter(tmp_path / 'bin')))
    assert code == 0, capsys.readouterr()
    target = set_world / 'chunks/0_0.glb'
    assert ops[:2] == [('remove', set_world / 'chunks.wci'), ('write', target)]
    assert ops[-1] == ('write', set_world / 'chunks.wci')
    assert (set_world / 'chunks/0_0.wcb').read_bytes() == fake_wcb(target)


@blender
def test_build_keeps_the_light_of_every_material(tmp_path, capsys):
    # Blender 5.2's exporter wrote COLOR_0 for the first material only (world_bpy.keep_vertex_light)
    base = world(tmp_path)
    pos, ids, tris = grid(0, 2)
    half = len(tris) // 2
    light = [0.2 + 0.05 * k for k in range(len(pos))]
    write_glb(base / 'worlds/TESTWORLD/chunks/0_0.glb', [(0, pos, ids, tris[:half], light),
                                                          (1, pos, ids, tris[half:], light)])
    code, staging, _ = build(tmp_path, base, '--set', 'test-1', '--chunk', 'TESTWORLD/0_0', *edit_args('add_post'))
    out = capsys.readouterr().out
    assert code == 0, out
    assert 'changed' in out and ', 0 changed' in out
    chunk = chunk_glb.read_chunk(staging / 'test-1/worlds/TESTWORLD/chunks/0_0.glb')
    new = [c for p in chunk.primitives for v, c in zip(p.vertex_ids.reshape(-1), p.colors.reshape(-1, 4)) if v < 0]
    # the post takes the light of the base around it, neither white nor black
    assert new and all(0.15 < c[0] < 0.7 for c in new)


# --- rebuilds of the published chunks

def published(record_id):
    entries = {e['id']: e for e in world_edit.read_registry(world_edit.REGISTRY)}
    entry = entries[record_id]
    try:
        base = world_edit.resolve_base(None)
    except world_edit.EditError:
        pytest.skip('no base')
    chunk = world_edit.STAGING / entry['files']['glb']['path']
    if not chunk.exists() or not (base / 'worlds').is_dir():
        pytest.skip(f'{chunk} is not on this machine')
    return entry, chunk


@blender
def test_rebuild_harbour_stairs_is_byte_equal(tmp_path):
    entry, chunk = published('harbour-stairs-newworld-1-1')
    assert world_edit.main(['build', entry['id'], '--dry-run', '--work', str(tmp_path)]) == 0
    assert world_edit.sha256(tmp_path / 'out.glb') == world_edit.sha256(chunk) == entry['files']['glb']['sha256']


@blender
def test_rebuild_roof_holes_is_the_same_geometry(tmp_path):
    entry, chunk = published('harbour-roof-holes-newworld-0-1')
    assert world_edit.main(['build', entry['id'], '--dry-run', '--work', str(tmp_path)]) == 0
    # the exact boolean orders the cut roof's vertices differently from run to run: compare the triangles
    assert chunk_glb.same_geometry(tmp_path / 'out.glb', chunk, full=True) == []
    assert world_edit.sha256(chunk) == entry['files']['glb']['sha256']


@blender
@pytest.mark.parametrize('record_id, hole', [('harbour-stairs-newworld-1-1', 'delete_largest_stair'),
                                             ('harbour-roof-holes-newworld-0-1', 'delete_hole_walls')])
def test_a_hole_in_the_new_geometry_is_caught(tmp_path, capsys, record_id, hole):
    entry, _ = published(record_id)
    args = ['build', 'test-hole', '--set', 'test-hole', '--chunk', entry['replaces'][0], '--from', record_id,
            '--edit', str(EDITS / f'{hole}.py'), '--dry-run', '--work', str(tmp_path)]
    for m in entry.get('materials_from', []):
        args += ['--materials', str(world_edit.STAGING / m)]
    assert world_edit.main(args) == 1
    assert 'FAIL: new open edge' in capsys.readouterr().out


def test_a_flat_source_triangle_is_the_same_whatever_its_normal():
    # the source has triangles with their corners on a line; Blender gives them a normal of its own
    import chunk_canon
    tri = np.zeros(3, chunk_canon.CORNER)
    tri['pos'] = [(-4739.36, -289.138, 2475.97), (-5614.36, -289.13775, 2475.97), (-5489.36, -289.13775, 2475.97)]
    tri['nrm'] = (0, -1, 0)
    tri['vid'] = tri['fid'] = [1, 2, 3]
    flipped = tri[[1, 2, 0]].copy()
    flipped['nrm'] = (0, 1, 0)
    assert chunk_canon._same(tri, flipped)
    tri['pos'][2] = (-5489.36, -250.0, 2475.97)       # not flat: the normal counts
    flipped = tri[[1, 2, 0]].copy()
    flipped['nrm'] = (0, 1, 0)
    assert not chunk_canon._same(tri, flipped)
