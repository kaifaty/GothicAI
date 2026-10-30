"""Tests of the world edit tool (world_edit.py, chunk_glb.py, world_bpy.py).

The check runs on synthetic chunks without Blender. The Blender tests build a synthetic world of two chunks and
apply the test edits of test_edits/; they skip without Blender. The rebuild tests compare the records of
data/content/world-edits.jsonl with the chunks published on this machine; they skip without the base or the sets.
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import chunk_glb  # noqa: E402
import world_edit  # noqa: E402

EDITS = HERE / 'test_edits'
MATERIALS = [{'name': 'TEST_A', 'texture': 'TEST_A.TGA'}, {'name': 'TEST_B', 'texture': 'TEST_B.TGA'}]


def write_glb(path, primitives, materials=MATERIALS):
    """A chunk as the converter writes it: per primitive POSITION, NORMAL, TEXCOORD_0, _VERTEX, indices; a
    primitive with a fifth item, the vertex light (0..1 per vertex), also gets _FEATURE and COLOR_0 (RGBA bytes)."""
    binary = bytearray()
    views, accessors, prims = [], [], []

    def add(array, kind, component, target=None, minmax=False):
        array = np.ascontiguousarray(array)
        while len(binary) % 4:
            binary.append(0)
        view = {'buffer': 0, 'byteOffset': len(binary), 'byteLength': array.nbytes}
        if target:
            view['target'] = target
        binary.extend(array.tobytes())
        views.append(view)
        acc = {'bufferView': len(views) - 1, 'componentType': component, 'count': len(array), 'type': kind}
        if minmax:
            acc['min'], acc['max'] = array.min(0).tolist(), array.max(0).tolist()
        accessors.append(acc)
        return len(accessors) - 1

    for material, positions, ids, triangles, *light in primitives:
        pos = np.array(positions, np.float32)
        n = len(pos)
        attrs = {'POSITION': add(pos, 'VEC3', 5126, 34962, True),
                 'NORMAL': add(np.tile(np.float32([0, 1, 0]), (n, 1)), 'VEC3', 5126, 34962),
                 'TEXCOORD_0': add(pos[:, [0, 2]] / 100, 'VEC2', 5126, 34962),
                 '_VERTEX': add(np.array(ids, np.float32), 'SCALAR', 5126, 34962)}
        if light:
            attrs['_FEATURE'] = add(np.array(ids, np.float32), 'SCALAR', 5126, 34962)
            grey = np.rint(np.array(light[0], np.float32) * 255).astype(np.uint8)
            attrs['COLOR_0'] = add(np.stack([grey, grey, grey, np.full(n, 255, np.uint8)], 1), 'VEC4', 5121, 34962)
            accessors[attrs['COLOR_0']]['normalized'] = True
        prims.append({'attributes': attrs, 'indices': add(np.array(triangles, np.uint32).reshape(-1), 'SCALAR', 5125, 34963),
                      'material': material, 'mode': 4})
    doc = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
           'nodes': [{'name': path.stem, 'mesh': 0, 'scale': [.01, .01, .01]}],
           'meshes': [{'name': path.stem, 'primitives': prims}],
           'materials': [{'name': m['name'], 'extras': {'gothic': {'index': i, **m}}} for i, m in enumerate(materials)],
           'accessors': accessors, 'bufferViews': views, 'buffers': [{'byteLength': len(binary)}]}
    text = json.dumps(doc).encode()
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    out = struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(text) + 8 + len(binary))
    out += struct.pack('<II', len(text), 0x4E4F534A) + text + struct.pack('<II', len(binary), 0x004E4942) + bytes(binary)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)


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


# --- Blender

def have_blender():
    try:
        return bool(world_edit.blender_binary())
    except world_edit.EditError:
        return False


blender = pytest.mark.skipif(not have_blender(), reason='no Blender')


def build(tmp_path, base, *args, record_id='test-chunk'):
    staging, registry = tmp_path / 'staging', tmp_path / 'world-edits.jsonl'
    argv = ['build', record_id, '--base', str(base), '--staging', str(staging), '--registry', str(registry),
            '--work', str(tmp_path / 'work'), *args]
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
