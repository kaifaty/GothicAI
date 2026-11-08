"""destruction_compare: retired pieces are not losses, and floats in water are counted apart."""
from __future__ import annotations

from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import destruction_compare as dc

BODY = 1001


def piece(index, sleeping=True, **fields):
    return {'parent_id': BODY, 'piece': index, 'sleeping': sleeping, **fields}


def rows(present, retired=()):
    final = {'kind': 'destruction_checkpoint', 'case': 'five', 'elapsed_ms': 30_003,
             'bodies': [{'id': BODY, 'broken': True, 'health': 0, 'max_health': 100}],
             'fragments': [piece(i) for i in present]}
    events = [{'kind': 'destruction_break', 'case': 'five', 'body': BODY, 'pieces': 10, 'break_ms': .1}]
    events += [{'kind': 'destruction_fragment_retired', 'case': 'five', 'parent_id': BODY, 'piece': i,
                'sleeping': True, 'reason': 'distant'} for i in retired]
    return events + [final], final


def test_retired_pieces_are_not_lost():
    events, final = rows(present=range(7), retired=(7, 8, 9))
    breaks, _ = dc.broken(events, 'five', final, 1)
    assert len(breaks) == 1


def test_piece_missing_without_cleanup_fails():
    events, final = rows(present=range(7), retired=(7, 8))
    with pytest.raises(ValueError, match=r'pieces \[9\] of 1001 lost without cleanup'):
        dc.broken(events, 'five', final, 1)


def test_float_in_water_is_counted_apart():
    sleep = {'kind': 'destruction_sleep15', 'case': 'five', 'elapsed_since_last_break_ms': 15_003,
             'fragments': [piece(0), piece(1), piece(2, sleeping=False, in_water=True, under_ground=False)]}
    final = {'elapsed_ms': 30_003, 'fragments': [piece(0), piece(1), piece(2, sleeping=True)]}
    asleep, summary = dc.asleep15([sleep], 'five', report=True, final=final)
    assert asleep
    assert summary['line'] == 'на суше спят 2/2, в воде 1 (спят к 30 с: 1/1), под землёй 0'

    sleep['fragments'][1]['sleeping'] = False
    asleep, summary = dc.asleep15([sleep], 'five', report=True, final=final)
    assert not asleep and summary['land_asleep'] == 1

    sleep['fragments'][1]['sleeping'] = True
    sleep['fragments'][2]['under_ground'] = True
    assert not dc.asleep15([sleep], 'five', report=True, final=final)[0]
