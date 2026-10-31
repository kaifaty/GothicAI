import importlib.util
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    'npc_character_virtual_compare', Path(__file__).parents[1] / 'npc_character_virtual_compare.py')
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)


def frame(sequence, time, **changes):
    row = dict.fromkeys(compare.SLIDING_FRAME_COLUMNS, 0)
    row.update(phase='stairs', npc_id=1, cv_active=True, moving=True, in_air=False,
               interactive=False, diagnostics_valid=True, move_sequence=sequence,
               move_dt_ms=50, t_game_ms=time, requested_x_cm=10, accepted_dx_cm=4,
               advancement_ratio=.4, steep_contact=True, steep_normal_x=-1)
    row.update(changes)
    return row


def measure(frames):
    run = {'frames': frames, 'pass': True, 'directory': 'fixture', 'complete': True,
           'missing': [], 'errors': []}
    return compare.steep_sliding(run, required_routes=('stairs',))


def test_sliding_duration_counts_unique_moves_and_splits_episodes():
    rows = [frame(1, 50), frame(1, 100), frame(2, 150),
            frame(3, 200, steep_contact=False), frame(4, 250)]
    result = measure(rows)
    route = result['routes'][0]
    assert result['pass'] is True
    assert route['cv_frames'] == 5
    assert route['cv_move_frames'] == 4
    assert route['sliding_frames'] == 3
    assert route['sliding_episodes'] == 2
    assert route['sliding_ms'] == 150
    assert route['longest_episode_ms'] == 100


def test_slope_stair_dynamic_contact_and_complete_block_are_not_sliding():
    rows = [frame(1, 50, steep_contact=False), frame(2, 100, advancement_ratio=.9),
            frame(3, 150, accepted_dx_cm=0, advancement_ratio=0),
            frame(4, 200, in_air=True), frame(5, 250, interactive=True)]
    result = measure(rows)['routes'][0]
    assert result['sliding_frames'] == 0
    assert result['sliding_episodes'] == 0
    assert result['sliding_ms'] == 0
    assert result['steep_below_threshold_frames'] == 1


def test_missing_diagnostics_remain_incomplete():
    row = frame(1, 50)
    del row['steep_contact']
    result = measure([row])
    assert result['pass'] is None
    assert result['routes'][0]['missing_diagnostic_frames'] == 1


def test_airborne_and_idle_samples_end_a_sliding_episode():
    rows = [frame(1, 50), frame(1, 100, moving=False), frame(2, 150)]
    result = measure(rows)['routes'][0]
    assert result['sliding_frames'] == 2
    assert result['sliding_episodes'] == 2
    assert result['sliding_ms'] == 100
