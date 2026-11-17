#!/usr/bin/env python3
"""Sheets of the short occlusion over the GI probes (../README.md) from two runs of contact-ambient.bench, without it
(OPENGOTHIC_GI_CONTACT_AO=0) and with it: every view as a crop of its two shots and what the occlusion took away
(the difference, brightened).

    uv run --no-project --with numpy --with pillow python contact_ambient_sheets.py <without> <with> <out dir> [view ...]

With --whole a view goes on its sheet uncropped.
"""
import importlib.util
import sys
from pathlib import Path

# the sheet itself is the one of the lamps' screen ray
_spec = importlib.util.spec_from_file_location(
    'lamp_contact_sheets',
    Path(__file__).resolve().parents[2] / '2026-11-15-night-interiors' / 'tools' / 'lamp_contact_sheets.py')
_sheets = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_sheets)

# view -> the box of the 2560x1440 shot that goes on the sheet
VIEWS = {
    'shade-feet': (700, 500, 1900, 1100),
    'shade-wide': (400, 200, 2200, 1300),
    'shade-face': (600, 100, 2000, 1300),
    'shade-game': (600, 300, 1900, 1300),
    'morning-feet': (700, 500, 1900, 1100),
    'table-day': (300, 150, 2000, 1100),
    'room-day': (0, 0, 2560, 1440),
    'cup-day': (600, 400, 2000, 1300),
    'tower-feet': (400, 300, 2200, 1300),
    'grass-low': (0, 300, 2560, 1440),
    'grass-noon': (0, 300, 2560, 1440),
}


def main() -> int:
    args = [a for a in sys.argv[1:] if a != '--whole']
    whole = '--whole' in sys.argv[1:]
    if len(args) < 3:
        print(__doc__)
        return 2
    _sheets.sheets(Path(args[0]), Path(args[1]), Path(args[2]), VIEWS, args[3:], whole,
                   labels=('no short AO', 'short AO'), prefix='contact-ambient-')
    return 0


if __name__ == '__main__':
    sys.exit(main())
