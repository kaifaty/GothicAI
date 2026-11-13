"""Tests of the region mode of the world edit tool (world_edit.py build-region, world_region.py, world_region_bpy.py).

A synthetic world of 4×4 chunks: the region 1..2 × 1..2 and a ring of outside chunks around it. The ground is one
grid of 100 cm quads over 9×9 cells with the chunk size 230 cm, so the chunk borders cut through the quads and every
triangle goes to the chunk of its centre, as the converter does; two materials (the seam at x = 500), vertex light
rising with x. The Blender tests apply the test edits test_edits/region_*.py; they skip without Blender.
"""
import json
import sys
from pathlib import Path

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import chunk_canon  # noqa: E402
import chunk_glb  # noqa: E402
import world_edit  # noqa: E402
import world_region  # noqa: E402
from world_fixtures import MATERIALS, blender, write_glb  # noqa: E402

EDITS = HERE / 'test_edits'
CELLS, STEP, SIZE = 9, 100.0, 230.0
REGION = 'TESTWORLD/1,1,2,2'
INSIDE = ['1_1', '1_2', '2_1', '2_2']
NEW = ('--set', 'test-r', '--region', REGION)


DEGENERATE = ((6, 3), (6, 3), (6, 4))     # in 2_1, on an inner edge
TWIN = ((5, 3), (6, 4), (6, 3))           # an inner triangle of 2_1, its duplicate the other way round


def light(i):
    return 0.2 + 0.06 * i


def feature(i, j, material, chunk):
    """_FEATURE of a corner: one vertex has another feature per material and per row of chunks, as the converter's
    vertices have on feature seams; so the seam x = 500 and the inner border z ≈ 460 cut features."""
    return 10 * i + j + 1000 * material + 2000 * (int(chunk.split('_')[1]) % 2)


def region_world(tmp_path):
    """The base: each chunk in the converter's form (one corner buffer, per triangle its order, flags, lightmap)."""
    base = tmp_path / 'base'
    chunks = base / 'worlds/TESTWORLD/chunks'
    by_chunk, order = {}, {}
    for i in range(CELLS):
        for j in range(CELLS):
            a, b, c, d = (i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)
            for tri in ((a, c, b), (a, d, c)):
                cx, cz = (sum(p[k] for p in tri) * STEP / 3 for k in (0, 1))
                name = f'{int(cx // SIZE)}_{int(cz // SIZE)}'
                by_chunk.setdefault(name, {}).setdefault(0 if i < 5 else 1, []).append(tri)
                order[tri] = len(order)
    # as the converter has them, two triangles Blender's importer drops: one with a corner twice and the triangle
    # TWIN of 2_1 again, the other way round
    for tri in (DEGENERATE, TWIN[::-1]):
        by_chunk['2_1'][1].append(tri)
        order[tri] = len(order)
    materials = [{'name': m['name'], 'extras': {'gothic': {'index': k, **m}}} for k, m in enumerate(MATERIALS)]
    for name, by_material in by_chunk.items():
        corners, index, prims = [], {}, []
        for material, tris in sorted(by_material.items()):
            for i, j in (p for t in tris for p in t):
                fid = feature(i, j, material, name)
                if (i, j, fid) not in index:
                    corner = np.zeros((), chunk_canon.CORNER)
                    corner['pos'], corner['nrm'], corner['uv'] = (STEP * i, 0, STEP * j), (0, 1, 0), (i, j)
                    corner['col'] = (*[round((light(i) + 0.02 * material) * 255)] * 3, 255)
                    corner['vid'], corner['fid'] = 10 * i + j, fid
                    index[(i, j, fid)] = len(corners)
                    corners.append(corner)
            prims.append((material, [[index[(*p, feature(*p, material, name))] for p in t] for t in tris],
                          np.array([order[t] for t in tris], np.float32),
                          np.array([[3, order[t] % 7] for t in tris], np.uint16), np.full(len(tris), -1, np.float32)))
        doc = {'asset': {'version': '2.0', 'extras': {'gothic': {'format': 'gothic-world-chunk'}}},
               'nodes': [{'name': name, 'mesh': 0, 'scale': [.01, .01, .01]}], 'materials': materials}
        chunks.mkdir(parents=True, exist_ok=True)
        chunk_canon._write(chunks / f'{name}.glb', doc, doc, np.array(corners, chunk_canon.CORNER), prims)
    (base / 'worlds/TESTWORLD/world.json').write_text(json.dumps({'materials': MATERIALS}))
    return base


def build_region(tmp_path, base, *args, record_id='test-region'):
    staging, registry = tmp_path / 'staging', tmp_path / 'world-edits.jsonl'
    argv = ['build-region', record_id, '--base', str(base), '--staging', str(staging), '--registry', str(registry),
            '--work', str(tmp_path / 'work'), *args]
    return world_edit.main(argv), staging, registry


def edit_args(*names):
    return [x for n in names for x in ('--edit', str(EDITS / f'{n}.py'))]


def chunk_paths(base, staging, name):
    return (base / f'worlds/TESTWORLD/chunks/{name}.glb', staging / f'test-r/worlds/TESTWORLD/chunks/{name}.glb')


def base_light(srcs, at, k=4):
    """The light bake_light gives a new vertex at `at`: the inverse-distance mean of the k nearest base vertices of
    the region (each the mean light of its corners in the triangles Blender holds). Several values when vertices tie
    for the last places, as on a grid."""
    import itertools
    corners, seen = {}, set()
    for src in srcs:
        for p in chunk_glb.read_chunk(src).primitives:
            for pos, vid, col in zip(p.positions, p.vertex_ids, p.colors):
                ids = frozenset(vid.tolist())
                if len(ids) < 3 or ids in seen:
                    continue          # a triangle Blender cannot hold: not in the underlay
                seen.add(ids)
                for q, v, c in zip(pos, vid, col):
                    corners.setdefault(int(v), (q, []))[1].append(c[0])
    points = np.array([q for q, _ in corners.values()], np.float64)
    lights = np.array([np.mean(cs) for _, cs in corners.values()])
    d = np.linalg.norm(points - at, axis=1)
    last = np.sort(d)[k - 1]
    sure = list(np.flatnonzero(d < last - 1e-3))
    tied = np.flatnonzero(np.abs(d - last) <= 1e-3)
    for extra in itertools.combinations(tied, k - len(sure)):
        idx = sure + list(extra)
        w = 1 / np.maximum(d[idx], 1e-3)
        yield float((w * lights[idx]).sum() / w.sum())


def inside_any(tris, point, eps=1e-3):
    """Is the point (x, z) inside one of the triangles (n, 3, 2)?"""
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]

    def side(o, u):
        return (u[:, 0] - o[:, 0]) * (point[1] - o[:, 1]) - (u[:, 1] - o[:, 1]) * (point[0] - o[:, 0])
    d = np.stack([side(a, b), side(b, c), side(c, a)])
    return bool((((d >= -eps).all(0)) | ((d <= eps).all(0))).any())


def test_the_synthetic_region_straddles_its_borders(tmp_path):
    base = region_world(tmp_path)
    chunks = base / 'worlds/TESTWORLD/chunks'
    assert world_region.region_chunks(chunks, (1, 1, 2, 2)) == INSIDE
    outside = world_region.outside_ids(chunks, INSIDE)
    # 24 (glTF 200, 0, 400) lies between the ring and the region; 55 (500, 0, 500) only between region chunks
    assert 24 in outside['1_1'] | outside['1_2'] and not any(55 in ids for ids in outside.values())
    one = chunk_glb.read_chunk(chunks / '1_1.glb')
    xs = np.concatenate([p.positions[:, :, 0] for p in one.primitives])
    assert xs.min() < SIZE < xs.max()            # its triangles reach over its border at x = 230


def test_a_t_junction_inside_a_region_fails_the_union(tmp_path):
    # two chunks share the edge (100, 0, 0) - (100, 0, 100); a's side of it is split at (100, 0, 50)
    pts = {'p': (0, 0, 0), 'q': (100, 0, 0), 'r': (100, 0, 100), 's': (0, 0, 100), 'm': (100, 0, 50),
           't': (200, 0, 0), 'u': (200, 0, 100)}
    ids = {k: n for n, k in enumerate(pts)}

    def chunk(path, tris):
        names = sorted({k for t in tris for k in t})
        write_glb(path, [(0, [pts[k] for k in names], [ids[k] for k in names],
                          [[names.index(k) for k in t] for t in tris])])
        return path
    a = chunk(tmp_path / 'a/0_0.glb', ['pqr', 'prs'])
    b = chunk(tmp_path / 'b/1_0.glb', ['qtu', 'qur'])
    a_split = chunk(tmp_path / 'a2/0_0.glb', ['pqm', 'pmr', 'prs'])
    summary, problems = world_region.union_open_edges([a, b], [a_split, b])
    assert summary['new'] == 3 and any('(100.00 0.00 0.00) - (100.00 0.00 50.00)' in p for p in problems)
    b_split = chunk(tmp_path / 'b2/1_0.glb', ['qtu', 'mur', 'qum'])
    summary, problems = world_region.union_open_edges([a, b], [a_split, b_split])
    assert summary['new'] == 0 and problems == []
    # an open edge of the base split by a new vertex is no opening, unless it is the region's outer border
    pts['n'], ids['n'] = (50, 0, 0), -1
    a_open = chunk(tmp_path / 'a3/0_0.glb', ['pnr', 'nqr', 'prs'])
    summary, problems = world_region.union_open_edges([a, b], [a_open, b])
    assert (summary['followed'], summary['new']) == (2, 0)
    summary, problems = world_region.union_open_edges([a, b], [a_open, b], outer={ids['p'], ids['q']})
    assert summary['new'] == 2 and any('(0.00 0.00 0.00) - (50.00 0.00 0.00)' in p for p in problems)


def test_a_chunk_belongs_to_one_record(tmp_path, capsys):
    base = region_world(tmp_path)
    registry = tmp_path / 'world-edits.jsonl'
    registry.write_text(json.dumps({'id': 'chunk-2-2', 'kind': 'worldchunk', 'replaces': ['TESTWORLD/2_2'],
                                    'files': {'glb': {'path': 'test-r/worlds/TESTWORLD/chunks/2_2.glb'}}}) + '\n')
    code, _, _ = build_region(tmp_path, base, *NEW, *edit_args('region_nothing'))
    assert code == 2 and 'chunk-2-2' in capsys.readouterr().err


# --- Blender

@blender
def test_a_region_edit_that_changes_nothing_publishes_nothing(tmp_path, capsys):
    base = region_world(tmp_path)
    code, staging, registry = build_region(tmp_path, base, *NEW, *edit_args('region_nothing'))
    out = capsys.readouterr().out
    assert code == 0, out
    assert 'TESTWORLD/2_1: source triangles from the base 13 of 14, new or changed 0; Blender cannot hold 2 as they ' \
        'were' in out
    assert '(17 feature seams closed)' in out
    assert not (staging / 'test-r').exists()
    for name in INSIDE:
        src, _ = chunk_paths(base, staging, name)
        assert world_edit.main(['compare', str(src), str(tmp_path / f'work/out/{name}.glb'), '--full']) == 0
    entry = json.loads(registry.read_text())
    assert entry['replaces'] == [] and entry['files'] == {'glb': []}
    assert [c['chunk'] for c in entry['chunks']] == [f'TESTWORLD/{n}' for n in INSIDE]


@blender
def test_an_inner_border_vertex_moves_in_every_chunk_with_it(tmp_path, capsys):
    base = region_world(tmp_path)
    code, staging, registry = build_region(tmp_path, base, *NEW, *edit_args('region_move_inner'))
    out = capsys.readouterr().out
    assert code == 0, out
    with_it = [n for n in INSIDE if 55 in chunk_glb.read_chunk(chunk_paths(base, staging, n)[0]).corners()[1]]
    assert len(with_it) >= 2
    entry = json.loads(registry.read_text())
    assert entry['replaces'] == [f'TESTWORLD/{n}' for n in with_it]
    for name in with_it:
        pos, vid = chunk_glb.read_chunk(chunk_paths(base, staging, name)[1]).corners()
        assert np.allclose(pos[vid == 55], [500, 10, 500])


@blender
def test_a_subdivided_region(tmp_path, capsys):
    base = region_world(tmp_path)
    code, staging, registry = build_region(tmp_path, base, *NEW, *edit_args('region_subdivide'))
    out = capsys.readouterr().out
    assert code == 0, out
    srcs, pubs = zip(*(chunk_paths(base, staging, n) for n in INSIDE))
    edges, problems = world_region.union_open_edges(srcs, pubs)
    assert edges['new'] == 0 and problems == []
    for src, pub in zip(srcs, pubs):
        b, e = chunk_glb.read_chunk(src), chunk_glb.read_chunk(pub)
        assert e.triangles > b.triangles
        # every child lies in a triangle of its parent's chunk, and together they cover it as the base did
        parents = np.concatenate([p.positions for p in b.primitives])[:, :, [0, 2]]
        assert all(inside_any(parents, tri.mean(0)[[0, 2]]) for p in e.primitives for tri in p.positions)
        areas = [{k: round(v['area_m2'], 6) for k, v in chunk_glb.material_table(c).items()} for c in (b, e)]
        assert areas[0] == areas[1]
        # the triangles it left are the base's byte for byte
        exact, bad = chunk_canon.source_violations(src, pub)
        assert bad == [] and exact > 0
        # a new vertex takes the inverse-distance mean of the base vertices around it, not the nearest one's light
        assert (e.corners()[1] < 0).sum() > 0
        for p in e.primitives:
            for v, c, at in zip(p.vertex_ids.reshape(-1), p.colors.reshape(-1, 4), p.positions.reshape(-1, 3)):
                if v < 0:
                    assert any(abs(c[0] - want) < 1.5 / 255 for want in base_light(srcs, at)), (at, c)
    entry = json.loads(registry.read_text())
    assert entry['kind'] == 'worldregion' and entry['region'] == REGION and entry['set'] == 'test-r'
    assert [e['script'] for e in entry['edits']] == ['scripts/content/world/test_edits/region_subdivide.py']
    for row, pub in zip(entry['chunks'], pubs):
        assert row['output'] == {'path': pub.relative_to(staging).as_posix(), 'sha256': world_edit.sha256(pub)}
        assert row['check']['light']['other'] == 0 and row['check']['light']['checked'] > 0
        assert row['base_sha256'] == world_edit.sha256(chunk_paths(base, staging, row['chunk'].split('/')[1])[0])
    assert entry['check']['open_edges']['new'] == 0

    # the same build again leaves the published chunks and the record as they are
    record, times = registry.read_text(), [p.stat().st_mtime_ns for p in pubs]
    code, _, _ = build_region(tmp_path, base)
    assert code == 0, capsys.readouterr().out
    assert registry.read_text() == record and [p.stat().st_mtime_ns for p in pubs] == times
    # a chunk of the region belongs to it
    code = world_edit.main(['build', 'test-chunk', '--set', 'test-r', '--chunk', 'TESTWORLD/1_1', '--base', str(base),
                            '--staging', str(staging), '--registry', str(registry), '--work', str(tmp_path / 'w2'),
                            *edit_args('region_nothing')])
    assert code == 2 and 'test-region' in capsys.readouterr().err


@blender
def test_the_outer_border_is_fixed(tmp_path, capsys):
    base = region_world(tmp_path)
    for name, reason in (('region_move_outer', 'FAIL: border vertex 24 moved to (200.00 10.00 400.00)'),
                         ('region_split_outer', 'FAIL: new open edge of the region (200.00 0.00 400.00) - '
                                                '(200.00 0.00 450.00)')):
        code, staging, registry = build_region(tmp_path, base, *NEW, *edit_args(name))
        out = capsys.readouterr().out
        assert code == 1 and reason in out, out
        assert not registry.exists() and not (staging / 'test-r').exists()


@blender
def test_a_face_from_nothing_needs_its_chunk(tmp_path, capsys):
    base = region_world(tmp_path)
    code, staging, registry = build_region(tmp_path, base, *NEW, *edit_args('region_face_from_nothing'))
    out = capsys.readouterr().out
    assert code == 1 and 'FAIL: a face at (296.67 50.00 296.67) has no chunk' in out, out
    assert not registry.exists()
    code, staging, registry = build_region(tmp_path, base, *NEW, *edit_args('region_face_in_chunk'))
    out = capsys.readouterr().out
    assert code == 0, out
    assert json.loads(registry.read_text())['replaces'] == ['TESTWORLD/2_2']
    src, pub = chunk_paths(base, staging, '2_2')
    pos, vid = chunk_glb.read_chunk(pub).corners()
    assert chunk_glb.read_chunk(pub).triangles == chunk_glb.read_chunk(src).triangles + 1
    assert np.allclose(np.sort(pos[vid < 0][:, 0]), [290, 290, 310])


@blender
def test_one_side_of_an_inner_border_and_of_a_feature_seam_is_split_for_both(tmp_path, capsys):
    # region_subdivide_part chooses faces of 1_1 and the TEST_A faces of 2_1 only: one surface splits their
    # neighbours across the inner borders and across the feature seam x = 500 along the shared edges
    base = region_world(tmp_path)
    code, staging, _ = build_region(tmp_path, base, *NEW, *edit_args('region_subdivide_part'))
    out = capsys.readouterr().out
    assert code == 0, out
    for name in ('1_2', '2_1'):
        src, pub = chunk_paths(base, staging, name)
        assert chunk_glb.read_chunk(pub).triangles > chunk_glb.read_chunk(src).triangles
    b = chunk_glb.read_chunk(chunk_paths(base, staging, '2_1')[1])
    assert any(p.material['name'] == 'TEST_B' and (p.vertex_ids < 0).any() for p in b.primitives)
    srcs, pubs = zip(*(chunk_paths(base, staging, n) for n in INSIDE))
    assert world_region.union_open_edges(srcs, pubs)[0]['new'] == 0


@blender
@pytest.mark.parametrize('edits, pieces, children', [(('region_lift',), 1, 1),
                                                     (('region_lift', 'region_subdivide'), 2, 4)])
def test_triangles_blender_cannot_hold_follow_the_edits(tmp_path, capsys, edits, pieces, children):
    base = region_world(tmp_path)
    code, staging, _ = build_region(tmp_path, base, *NEW, *edit_args(*edits))
    out = capsys.readouterr().out
    assert code == 0, out
    assert f'Blender cannot hold 0 as they were, 2 moved as {pieces + children} triangles' in out
    chunk = chunk_glb.read_chunk(chunk_paths(base, staging, '2_1')[1])
    tris = [tri for p in chunk.primitives for tri in p.positions]
    keys = [[tuple(c) for c in chunk_glb.position_keys(t)] for t in tris]
    # the triangle with a corner twice: at its new place, split as its edge was
    flat = [t for t, k in zip(tris, keys) if len(set(k)) < 3]
    assert len(flat) == pieces and all(np.allclose(t[:, 1], 1) for t in flat)
    xs = sorted({float(z) for t in flat for z in t[:, 2]})
    assert xs[0] == 300 and xs[-1] == 400
    # the duplicate: its twin's children again, the other way round
    turns = [[b[0], b[2], b[1]] for b in keys]
    turned = sum(1 for a in keys for r in turns if len(set(a)) == 3 and any(a == r[k:] + r[:k] for k in range(3)))
    assert turned == 2 * children


def test_errors_end_as_messages(tmp_path):
    # run as a script, world_edit.py is __main__: world_region's errors still end as 'world_edit: ...', exit 2
    import subprocess
    base = region_world(tmp_path)
    registry = tmp_path / 'world-edits.jsonl'
    registry.write_text(json.dumps({'id': 'test-region', 'kind': 'worldregion', 'set': 'test-r', 'region': REGION,
                                    'chunks': [{'chunk': 'TESTWORLD/1_1'}], 'replaces': [],
                                    'files': {'glb': []}}) + '\n')

    def run(*args):
        result = subprocess.run([sys.executable, str(HERE / 'world_edit.py'), *args, '--base', str(base),
                                 '--registry', str(registry), '--staging', str(tmp_path / 'staging'),
                                 '--work', str(tmp_path / 'work')], capture_output=True, text=True)
        return result.returncode, result.stderr
    nothing = ['--edit', str(EDITS / 'region_nothing.py')]
    cases = [(['build-region', 'x', '--set', 's', '--region', 'BAD', *nothing], "world_edit: region 'BAD'"),
             (['build', 'test-region'], 'world_edit: test-region is a region record: use build-region'),
             (['build', 'x', '--from', 'test-region', '--set', 'test-r2', '--chunk', 'TESTWORLD/1_1'],
              'world_edit: --from test-region is a region record'),
             (['build', 'x', '--set', 'test-r', '--chunk', 'TESTWORLD/1_1', *nothing],
              'world_edit: TESTWORLD/1_1 of test-r belongs to the record test-region')]
    for args, message in cases:
        code, err = run(*args)
        assert code == 2 and message in err and 'Traceback' not in err, (args, err)
