"""Real holes in every shingle roof NW_CITY_ROOF_01 of Khorinis: the pilot of the harbour hut B037 (roof_holes.py)
carried over to the whole city (set cityroofs-1, chunks 0_-2, 0_-1, 0_0, 1_-1, 1_0).

    python3 scripts/content/world/world_edit.py build city-roofs-newworld-0-1 --set cityroofs-1 \
        --chunk NEWWORLD/0_-1 --edit scripts/content/world/city_roof_holes.py \
        --edit scripts/content/world/city_roof_edge.py \
        --materials <staging>/roofhole-materials-2/materials/materials.json

The texture paints its five holes on every roof that carries it: the huts and the brothel of the harbour, the
harbour tavern, Matteo's shop, the houses at the market, the town tavern, the warehouse. Every one is cut where it
is painted, as on B037. What the pilot did not meet:
- the big roofs span several squares of the texture on one panel: every copy of the holes on it is cut;
- a panel the chunk border split: a hole that runs off its triangles stays painted;
- a face looking down (the underside of an eave, the lighthouse's only ROOF_01) gets no hole;
- a lath or a rafter whose top corner is not under the shingles is left out: the big roofs bend between triangles,
  and a straight beam laid in one triangle's plane stood out of the next one (render, 2026-11-02);
- a roof the player does not go under has no inside skin: the hole gets a closed bottom, the dark of the attic
  (OWN_ROOF_HOLE_DARK, roof_hole_shade.py), not an opening onto the sky through the house; an inside skin farther
  than MAX_GAP under a hole is another building's.

The converted chunk is Gothic content (a derivative, user's decision 2026-10-24): it stays outside git.
"""
from pathlib import Path

import roof_holes

CITY = (-2500.0, 14000.0, -8000.0, 18000.0)   # Gothic cm (x0, x1, z0, z1): the town from the lighthouse to the market
MAX_GAP = 80.0     # cm: the huts' skins are 15-55 cm apart (roof_holes.py on B037)
INPUTS = [Path(roof_holes.__file__).resolve()] + roof_holes.INPUTS
PARAMETERS = dict(roof_holes.PARAMETERS, box_cm=list(CITY), tiles=True, dark=roof_holes.DARK,
                  closed_depth_cm=roof_holes.CLOSED_DEPTH, max_gap_cm=MAX_GAP)


def edit(ctx):
    roof_holes.edit(ctx, box=CITY, tiles=True, dark=roof_holes.DARK, max_gap=MAX_GAP)
