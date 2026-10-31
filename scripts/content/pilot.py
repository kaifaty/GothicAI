"""Shared city-set scope and placement rules; no image inputs."""

TARGET_LUMINANCE_TOLERANCE = 0.002
CITY_2_SETS = frozenset({"city-2", "city-2-imagegen"})
CITY_3_SET = "city-3"
CITY_3_DECISIONS = frozenset({"include", "module", "panel", "strip"})
CITY_3_EXCLUDED_CLASSES = frozenset({
    "iron_panel", "banded_metal_tile", "iron_strip",
})
PLACEMENT_SETS = CITY_2_SETS | {CITY_3_SET}

CITY_2_MODULES = (
    "NW_CITY_WALL_03.TGA",
    "NW_CITY_WINDOW_03.TGA",
    "NW_CITY_FACHWERKBASEMENT_WINDOW_01.TGA",
    "NW_CITY_FACHWERKBASEMENT_CHAIN_01.TGA",
    "NW_CITY_FACHWERKBASEMENT_EMPTY_01.TGA",
    "NW_CITY_WOODBEAM_02.TGA",
    "NW_CITY_ROOF_01.TGA",
    "NW_CITY_HAFEN_HOLZWAND_01.TGA",
)

# Physical U/V proportions of the architectural repeat, before square UV packing.
MODULE_ASPECT_RATIOS = {
    "NW_CITY_WALL_03.TGA": 2.00 / 2.06,
    "NW_CITY_WINDOW_03.TGA": 4.00 / 3.75,
    "NW_CITY_FACHWERKBASEMENT_WINDOW_01.TGA": 1.0,
    "NW_CITY_FACHWERKBASEMENT_CHAIN_01.TGA": 2.95 / 2.61,
    "NW_CITY_FACHWERKBASEMENT_EMPTY_01.TGA": 1.0,
    "NW_CITY_WOODBEAM_02.TGA": 1.15 / 10.14,
}
MODULE_REPEAT_AXES = {"NW_CITY_WOODBEAM_02.TGA": "v"}

ANTITILE_BY_CLASS = {
    "cobble": "rotate", "flagstone": "rotate", "masonry_dressed": "offset",
    "masonry_rubble": "rotate", "plaster": "rotate", "planks_floor": "offset",
    "planks_wall": "offset", "timber_beam": "offset", "roof_shingle": "offset",
    "roof_tiles": "offset", "thatch": "offset", "dirt_path": "rotate", "mud": "rotate",
}


def selected_rows(rows, set_name):
    """Keep CSV order and use one scope for generation, packing and measurement."""
    # The market/wall additions are preparation targets, outside existing sets.
    rows = [row for row in rows if not row.get("texture", "").startswith("NW_MISC_")]
    if set_name == CITY_3_SET:
        selected = [row for row in rows if row.get("decision") in CITY_3_DECISIONS
                    and row.get("class") not in CITY_3_EXCLUDED_CLASSES]
    else:
        selected = [row for row in rows if row.get("decision") == "include"
                    or (set_name in CITY_2_SETS and row.get("texture") in CITY_2_MODULES)]
    names = [row["texture"] for row in selected]
    if len(names) != len(set(names)):
        raise ValueError(f"mapping repeats selected texture in {set_name}")
    return selected


def antitile_mode(row):
    """Homogeneous surfaces use class policy; authored modules keep their UVs."""
    return ANTITILE_BY_CLASS[row["class"]] if row["decision"] == "include" else "off"
