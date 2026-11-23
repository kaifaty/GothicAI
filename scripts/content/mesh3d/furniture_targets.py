#!/usr/bin/env python3
"""Measure scalar targets for the house furniture set (asset city-furniture): chairs, beds, cupboards, thrones, tables.

    python3 scripts/content/mesh3d/furniture_targets.py --base DIR [--output data/content/mesh3d-furniture.json]
                                                        [--underlay DIR]

The record shape, the units and the rule that only names and numbers leave the base are hearth_targets.py's:
static visuals from meshes/<VISUAL>.glb, mob models from models/ in their bind pose with their ZS_POS* nodes
(where an npc sits, lies or opens a cupboard). Zones are first guesses for the design step, which may change them.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from hearth_targets import BASE_CACHE, ROOT, measure, zone

WOOD = zone('OWN_FURN_WOOD', 'WOOD', 'rough dark pine of plain furniture: stools, plain beds, dressers, tables')
DARK = zone('OWN_FURN_DARKWOOD', 'WOOD', 'dark oiled oak of rich furniture, carved parts')
OAK = zone('OWN_FURN_OAK', 'WOOD', 'the lighter oiled oak top of the rich table')
IRON = zone('OWN_FURN_IRON', 'METAL', 'forged bands, strap hinges, brackets, rivet heads, handles')
CLOTH = zone('OWN_FURN_LINEN', 'UNDEF', 'pale linen of rich beds and the canopy')
TRIM = zone('OWN_FURN_TRIM', 'UNDEF', 'the woven green border stripe of the rich linen')
STRAW = zone('OWN_FURN_STRAW', 'UNDEF', 'the coarse olive cloth of a straw mattress with patches')
LEATHER = zone('OWN_FURN_LEATHER', 'UNDEF', 'red-brown leather of the armchair, book covers')
BRASS = zone('OWN_FURN_BRASS', 'METAL', 'darkened brass lion bosses')
PAPER = zone('OWN_FURN_PAPER', 'UNDEF', 'page edges and open pages of a book')
STONE = zone('OWN_FURN_STONE', 'STONE', 'dressed stone blocks of the bench pedestals')

TARGETS = [
    {'id': 'furn-chair', 'visual': 'CHAIR_NW_NORMAL_01', 'budget_tris': 4000, 'zones': [WOOD, IRON]},
    {'id': 'furn-bed', 'visual': 'BEDHIGH_NW_NORMAL_01', 'budget_tris': 8000, 'zones': [WOOD, STRAW]},
    {'id': 'furn-cupboard-rich-2', 'visual': 'NW_CITY_CUPBOARD_RICH_02', 'budget_tris': 12000, 'zones': [DARK, IRON]},
    {'id': 'furn-cupboard-poor', 'visual': 'NW_CITY_CUPBOARD_POOR_01', 'budget_tris': 14000, 'zones': [WOOD, IRON]},
    {'id': 'furn-throne', 'visual': 'THRONE_NW_CITY_01', 'budget_tris': 16000, 'zones': [LEATHER, BRASS]},
    {'id': 'furn-cupboard-rich-1', 'visual': 'NW_CITY_CUPBOARD_RICH_01', 'budget_tris': 22000, 'zones': [DARK, IRON]},
    {'id': 'furn-chair-rich', 'visual': 'CHAIR_NW_EDEL_01', 'budget_tris': 5000, 'zones': [DARK, IRON]},
    {'id': 'furn-bed-rich', 'visual': 'BEDHIGH_NW_EDEL_01', 'budget_tris': 10000, 'zones': [DARK, CLOTH, TRIM]},
    {'id': 'furn-bed-master', 'visual': 'BEDHIGH_NW_MASTER_01', 'budget_tris': 20000, 'zones': [DARK, CLOTH, TRIM]},
    {'id': 'furn-table-rich', 'visual': 'NW_CITY_TABLE_RICH_02', 'budget_tris': 14000, 'zones': [DARK, OAK, IRON]},
    {'id': 'furn-table', 'visual': 'NW_CITY_TABLE_NORMAL_01', 'budget_tris': 6000, 'zones': [WOOD, IRON]},
    {'id': 'furn-bench-2', 'visual': 'BENCH_NW_CITY_02', 'budget_tris': 5000, 'zones': [WOOD, STONE]},
    {'id': 'furn-bookshelf', 'visual': 'BOOK_NW_CITY_CUPBOARD_01', 'budget_tris': 14000, 'zones': [DARK, IRON, LEATHER, PAPER]},
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'data/content/mesh3d-furniture.json')
    parser.add_argument('--underlay', type=Path)
    args = parser.parse_args()
    base = args.base.resolve()
    targets = [measure(base, t, args.underlay) for t in TARGETS]
    args.output.write_text(json.dumps({'base': base.name, 'targets': targets}, indent=1, ensure_ascii=False) + '\n')
    print(args.output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
