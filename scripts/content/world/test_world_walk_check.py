"""Tests of the walkability check (world_walk_check.py).

Synthetic worlds: one chunk of 1 m ground tiles over Gothic x, z 0..16 m at height 0, a second layer 5 m up over
x 0..4, z 12..16, three waypoints and one tree; the content folder replaces the chunk with changed tiles. The last
test runs the real base against an empty content folder and skips without it.
"""
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import world_walk_check as walk  # noqa: E402
from test_world_census import chunk, make_world, square  # noqa: E402

# Gothic cm. P0 -> P1 runs along z = 8.5 m; P2 stands 0.5 m above tile (5, 3); the tree is on tile (10, 3).
POINTS = {'P0': (200, 50, 850), 'P1': (1400, 50, 850), 'P2': (550, 50, 350)}
TREE = (7, 'NW_NATURE_TANNE_75P.3DS', (1050, 0, 350))
UPPER = square(0, 12, 4, y=5)


def tiles(height=lambda i, j: 0.0):
    return [square(i, j, 1, y=height(i, j)) for i in range(16) for j in range(16)]


def world(tmp_path, height=None, upper=(UPPER,)):
    """Base and content: the content chunk has tiles at height(i, j) and the upper squares given."""
    base = make_world(tmp_path / 'base', {'0_0': tiles() + [UPPER]}, vobs=[TREE])
    points = [{'name': n, 'position': list(p), 'free_point': False} for n, p in POINTS.items()]
    (base / 'worlds' / 'TEST' / 'waynet.json').write_text(json.dumps(
        {'format': 1, 'waynet': {'points': points, 'edges': [[0, 1]]}}))
    content = tmp_path / 'content'
    content.mkdir()
    if height is not None or tuple(upper) != (UPPER,):
        chunk(content / 'worlds' / 'TEST' / 'chunks' / '0_0.glb', *(tiles(height) if height else tiles()), *upper)
    return base, content


def check(base, content):
    return walk.walk_check(base, 'TEST', content)


def raised(cells, dy):
    return lambda i, j: dy if (i, j) in cells else 0.0


def test_an_empty_content_folder_changes_nothing(tmp_path):
    R = check(*world(tmp_path))
    assert R['passed'] and R['chunks_with_differences'] == []
    assert R['waypoints']['change_m']['n'] == 3 and R['waypoints']['change_m']['abs_max'] == 0
    assert R['edges']['samples'] == 49 and R['edges']['changed'] == 0
    assert R['edges']['height_change_m'] == {**R['edges']['height_change_m'], 'n': 1, 'nonzero': 0, 'abs_max': 0}
    assert R['ground']['layers_a'] == 256 + 16 and R['ground']['change_m']['nonzero'] == 0
    assert R['objects']['change_m'] == {**R['objects']['change_m'], 'n': 1, 'nonzero': 0}


def test_a_bump_under_a_waypoint_is_flagged_with_its_size(tmp_path):
    R = check(*world(tmp_path, raised({(5, 3)}, 0.06)))
    over = R['waypoints']['over_tolerance']
    assert [(w['name'], w['change_m']) for w in over] == [('P2', 0.06)]
    assert [(w['name'], w['ground_a_m'], w['ground_b_m']) for w in R['waypoints']['rows']] == \
        [('P0', 0.0, 0.0), ('P1', 0.0, 0.0), ('P2', 0.0, 0.06)]
    assert [(e['from'], e['height_change_m'], e['fails']) for e in R['edges']['rows']] == [('P0', 0.0, [])]
    assert R['waypoints']['below_surface'] == [] and not R['passed']
    assert R['ground']['over_tolerance_m2'] == 0 and R['ground']['change_m']['nonzero'] == 1


@pytest.mark.parametrize('lower, ground_b', [((), None), ((square(5, 3, 1, y=-1),), -1.0)])
def test_ground_raised_over_the_stored_position_starts_the_ray_below_the_surface(tmp_path, lower, ground_b):
    R = check(*world(tmp_path, raised({(5, 3)}, 0.8), upper=(UPPER, *lower)))
    [w] = R['waypoints']['below_surface']
    assert (w['name'], w['depth_m'], w['ground_b_m']) == ('P2', 0.3, ground_b)
    assert R['failures']['waypoints_below_surface'] == 1


@pytest.mark.parametrize('step, fails', [(0.62, True), (0.58, False)])
def test_a_step_on_an_edge_fails_it_by_step_height(tmp_path, step, fails):
    R = check(*world(tmp_path, raised({(i, 8) for i in range(8, 12)}, step)))
    [e] = R['edges']['failed']
    assert (e['from'], e['to'], e['step_a_m'], e['step_b_m'], e['slope_b_deg']) == ('P0', 'P1', 0.0, step, 0.0)
    assert (f'step {step:.2f} m' in e['fails']) == fails
    assert R['ground']['over_tolerance_m2'] == 4


def test_a_new_platform_within_the_step_band_is_the_new_path(tmp_path):
    base, content = world(tmp_path, upper=(UPPER, square(8, 8, 4, y=0.2)))
    R = check(base, content)
    assert R['edges']['failed'][0]['fails'] == ['height 0.20 m']


def test_ground_moved_0_2_m_is_inside_the_ground_tolerance_but_fails_the_edge(tmp_path, capsys):
    base, content = world(tmp_path, lambda i, j: 0.2)
    out = tmp_path / 'out'
    assert walk.main(['TEST', '--base', str(base), '--content', str(content), '--out', str(out)]) == 1
    R = json.loads((out / 'TEST_walk.json').read_text())
    g = R['ground']
    assert (g['over_tolerance_m2'], g['change_m']['nonzero'], g['change_m']['max']) == (0, 256, 0.2)
    assert R['edges']['failed'][0]['fails'] == ['height 0.20 m']
    assert len(R['waypoints']['over_tolerance']) == 3
    assert (out / 'TEST_walk.md').read_text() + '\n' == capsys.readouterr().out
    assert walk.main(['TEST', '--base', str(base), '--content', str(content), '--path-tolerance', '0.25']) == 0
    R = check(*world(tmp_path / 'higher', lambda i, j: 0.32))
    assert R['ground']['over_tolerance_m2'] == 256


@pytest.mark.parametrize('dy', [-0.06, 0.2])
def test_ground_moved_under_an_object_lists_it_in_its_own_file(tmp_path, dy):
    base, content = world(tmp_path, upper=(UPPER, square(10, 3, 1, y=dy)) if dy > 0 else (UPPER,),
                          height=raised({(10, 3)}, dy if dy < 0 else 0.0))
    out = tmp_path / 'out'
    assert walk.main(['TEST', '--base', str(base), '--content', str(content), '--out', str(out)]) == 0
    [o] = json.loads((out / 'TEST_walk_objects.json').read_text())['objects']
    assert (o['id'], o['visual'], o['position_cm'], o['change_m']) == (7, TREE[1], list(TREE[2]), dy)


def test_a_removed_layer_and_an_added_layer(tmp_path):
    R = check(*world(tmp_path, lambda i, j: 0.0, upper=[square(8, 12, 4, y=5)]))
    g = R['ground']
    assert (g['vanished_layers'], g['appeared_layers'], g['change_m']['nonzero']) == (16, 16, 0)
    assert [(c['chunk'], c['vanished'], c['appeared']) for c in g['chunks']] == [('0_0', 16, 16)]
    assert not R['passed']
    g = check(*world(tmp_path / 'shifted', lambda i, j: 0.0, upper=[square(0, 12, 4, y=6.5)]))['ground']
    assert (g['vanished_layers'], g['appeared_layers'], g['change_m']['max']) == (0, 0, 1.5)


def test_newworld_against_an_empty_content_folder_is_all_zero(tmp_path):
    try:
        base = walk.census.resolve_base(None)
    except SystemExit:
        pytest.skip('no base')
    if not (base / 'worlds' / 'NEWWORLD' / 'waynet.json').is_file():
        pytest.skip('no NEWWORLD in the base')
    R = walk.walk_check(base, 'NEWWORLD', tmp_path, walk.census.parse_rect('0,-2,1,0'))
    assert R['passed'] and R['chunks_with_differences'] == []
    for part in (R['waypoints'], R['ground'], R['objects']):
        assert part['change_m']['n'] > 0 and part['change_m']['nonzero'] == 0
    e = R['edges']
    assert e['edges'] > 0 and e['changed'] == 0 and e['height_change_m']['nonzero'] == 0
    assert e['height_change_m']['abs_max'] == 0 and len(e['rows']) == e['edges']
