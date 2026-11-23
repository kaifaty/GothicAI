"""The edge of every shingle roof NW_CITY_ROOF_01 of Khorinis: the butts and the thickness of the shingles of the
harbour huts (roof_edge.py, M2) carried over to the whole city (set cityroofs-1); it goes after city_roof_holes.py.

The eaves and the verges are found as on the huts: by the panel's texture mapping, not by the building. An edge
the chunk border made is no edge (the roof goes on in the next chunk) and stays as it is.

The converted chunk is Gothic content (a derivative, user's decision 2026-10-24): it stays outside git.
"""
from pathlib import Path

import roof_edge
from city_roof_holes import CITY

INPUTS = [Path(roof_edge.__file__).resolve()] + roof_edge.INPUTS
PARAMETERS = dict(roof_edge.PARAMETERS, boxes_cm={'city': list(CITY)}, joints=True)


def edit(ctx):
    roof_edge.edit(ctx, boxes={'city': CITY}, joints=True)
