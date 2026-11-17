"""The cells of a converted world's world.json, as the engine reads them (OpenGothic/common/worldchunkmanifest.*).

An entry of "chunks" is a string, one draw level 0 mesh in the cell of its file stem <x>_<z>, or an object
{"cell": [x, z], "meshes": [{"file": ..., "role": "draw", "level": 0}, {"file": ..., "role": "collision"}]} with
exactly one draw level 0 mesh. The tools read the draw level 0 mesh of each cell, as the engine does: it is what
the game draws and, until Д4 of docs/landscape-plan.md, what its collision is built from. A "collision" mesh and
the other draw levels are listed in world.json only; nothing reads them yet.

Shared by world_census.py (and through it world_walk_check.py) and world_cook.py.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Cell:
    name: str      # '<x>_<z>': the file stem of a string entry, the cell of an object entry
    draw: str      # the draw level 0 file, relative to the world folder


def cells(world: Path | dict) -> list[Cell]:
    """The cells of world.json (a world folder, the file or its parsed object) in manifest order."""
    if isinstance(world, Path):
        path = world / 'world.json' if world.is_dir() else world
        world = json.loads(path.read_text())
    out = []
    for i, entry in enumerate(world['chunks']):
        if isinstance(entry, str):
            out.append(Cell(Path(entry).stem, entry))
            continue
        cell = entry.get('cell') if isinstance(entry, dict) else None
        if not (isinstance(cell, list) and len(cell) == 2 and all(isinstance(v, int) for v in cell)):
            raise ValueError(f'world.json chunks entry {i}: cell must be two integers [x, z]')
        draw = [m['file'] for m in entry.get('meshes', [])
                if m.get('role') == 'draw' and m.get('level', 0) == 0]
        if len(draw) != 1:
            raise ValueError(f'world.json chunks entry {i}, cell {cell[0]},{cell[1]}: '
                             f'{len(draw)} draw level 0 meshes, expected one')
        out.append(Cell(f'{cell[0]}_{cell[1]}', draw[0]))
    return out
