"""Synthetic census fixtures cover the city mapping contract without game pixels."""

from copy import deepcopy
import csv
from decimal import Decimal
import importlib.util
from pathlib import Path
import subprocess
import sys

import pytest


SCRIPT = Path(__file__).with_name("map_textures.py")
SPEC = importlib.util.spec_from_file_location("map_textures", SCRIPT)
mapping = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mapping)
CENSUS_FIELDS = ("world", "texture", "area_m2", "uv_m_per_unit_median", "tiled_area_fraction", "vob_count")


def write_csv(path, fields, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    return path


@pytest.fixture
def dataset(tmp_path):
    source = [{
        "world": "NEWWORLD.ZEN", "texture": f"NW_CITY_TEST_{index:02d}.TGA",
        "area_m2": "10.25", "uv_m_per_unit_median": "5.01",
        "tiled_area_fraction": "0.8", "vob_count": "0",
    } for index in range(71)]
    source[-1]["texture"] = "NW_HARBOUR_TEST.TGA"
    rows = [{
        "texture": row["texture"], "decision": "include", "class": "cobble",
        "state": "very worn", "scale": "2.51", "area_m2": row["area_m2"],
        "uv_m_per_unit_median": row["uv_m_per_unit_median"], "reason": "tileable surface",
        "object": "town street", "role": "walking surface", "depicts": "small stone paving",
        "colour": "warm grey", "weathering": "dirt between stones", "orientation": "no fixed top",
        "layout": "tiled", "layout_share": "tiled:0.8;panel:0.1;strip:0.1;atlas:0",
        "anti_repeat": "on", "target_luminance": "0.12", "target_coverage": "",
        "target_period_u_uv": "0.125", "target_period_v_uv": "0.25",
        "target_period_u_m": "0.62625", "target_period_v_m": "1.2525",
        "target_direction": "none", "target_direction_confidence": "0.1",
        "target_oklab_l": "0.5", "target_oklab_a": "-0.02", "target_oklab_b": "0.03",
        **dict.fromkeys(mapping.DETAIL_FIELDS, "0.2"),
        **dict.fromkeys(mapping.COUNT_FIELDS, ""),
    } for row in source]
    rows[-4].update(decision="module", **{"class": "timber_frame_tile", "scale": "1", "anti_repeat": "off"})
    rows[-3].update(decision="panel", **{"class": "plaster_panel", "scale": "1",
        "layout": "panel", "layout_share": "tiled:0.1;panel:0.8;strip:0.1;atlas:0", "anti_repeat": "off"})
    rows[-2].update(decision="strip", **{"class": "timber_strip", "scale": "1",
        "layout": "strip", "layout_share": "tiled:0.1;panel:0.1;strip:0.8;atlas:0", "anti_repeat": "off"})
    rows[-1].update(decision="exclude", **{"class": "", "state": "", "scale": "", "reason": "object atlas",
        "layout": "atlas", "layout_share": "tiled:0;panel:0;strip:0;atlas:1", "anti_repeat": "off"})
    census_path = tmp_path / "census.csv"
    mapping_path = tmp_path / "materials.csv"

    def save():
        write_csv(census_path, CENSUS_FIELDS, source)
        write_csv(mapping_path, mapping.FIELDS, rows)

    save()
    return source, rows, census_path, mapping_path, save


def test_scale_uses_uv_divided_by_tile_and_decimal_rounding():
    assert mapping.material_scale(Decimal("5.01"), "cobble") == Decimal("2.51")
    assert mapping.material_scale(Decimal("5.01"), "flagstone") == Decimal("1.67")
    assert mapping.material_scale(Decimal("5.01"), "timber_beam") == Decimal("3.34")


@pytest.mark.parametrize("field,value", [
    ("target_period_u_uv", "NaN"), ("target_period_v_uv", "1.01"),
    ("target_period_u_m", "0.7"), ("target_direction", "horizontal"),
    ("target_direction_confidence", "1.1"), ("target_oklab_l", ""),
    ("target_oklab_a", "-0.51"), ("target_oklab_b", "Infinity"),
])
def test_check_requires_valid_original_material_measurements(dataset, field, value):
    source, rows, census_path, mapping_path, save = dataset
    rows[0][field] = value
    save()
    with pytest.raises(mapping.MappingError, match=field):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("field", mapping.DETAIL_FIELDS)
@pytest.mark.parametrize("value", ["", "NaN", "Infinity", "-0.1"])
@pytest.mark.parametrize("index", [0, -1])
def test_all_rows_require_finite_nonnegative_detail_targets(dataset, field, value, index):
    _, rows, census_path, mapping_path, save = dataset
    rows[index][field] = value
    save()
    with pytest.raises(mapping.MappingError, match=field):
        mapping.check_mapping(mapping_path, census_path)


def test_update_preserves_original_measurements_and_recomputes_meters(dataset):
    source, rows, census_path, mapping_path, save = dataset
    before = deepcopy(rows)
    source[0]["uv_m_per_unit_median"] = "6"
    save()
    updated = mapping.update_mapping(mapping_path, census_path)
    assert updated[0]["target_period_u_m"] == "0.750"
    assert updated[0]["target_period_v_m"] == "1.50"
    for field in mapping.MEASUREMENT_FIELDS:
        if not field.endswith("_m"):
            assert [row[field] for row in updated] == [row[field] for row in before]


def test_complete_scope_and_cli_summary_are_read_only(dataset, capsys):
    source, rows, census_path, mapping_path, save = dataset
    source.extend([
        dict(source[0], texture="OTHER.TGA"),
        dict(source[0], world="OLDWORLD.ZEN"),
    ])
    save()
    before = mapping_path.read_bytes()
    assert mapping.check_mapping(mapping_path, census_path) == rows
    assert mapping.main(["check", "--mapping", str(mapping_path), "--census", str(census_path)]) == 0
    assert mapping_path.read_bytes() == before
    output = capsys.readouterr().out
    assert "rows=71 include=67 panel=1 strip=1 module=1 exclude=1" in output
    assert "686.75 / 727.75 m2 (94.37%)" in output


@pytest.mark.parametrize("field,value", [
    ("area_m2", "10.26"), ("uv_m_per_unit_median", "5.02"), ("scale", "0.40"),
])
def test_check_rejects_stale_derived_values(dataset, field, value):
    source, rows, census_path, mapping_path, save = dataset
    rows[0][field] = value
    save()
    with pytest.raises(mapping.MappingError, match=f"stale {field}"):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("change,message", [
    ("missing", "missing textures"), ("extra", "extra textures"), ("duplicate", "duplicate mapping texture"),
])
def test_mapping_must_cover_scope_exactly(dataset, change, message):
    source, rows, census_path, mapping_path, save = dataset
    if change == "missing":
        rows.pop()
    elif change == "extra":
        rows.append(dict(rows[0], texture="NW_CITY_EXTRA.TGA"))
    else:
        rows.append(deepcopy(rows[0]))
    save()
    with pytest.raises(mapping.MappingError, match=message):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("change,message", [
    ("missing", "expected 71 city/harbour textures, got 70"),
    ("extra", "expected 71 city/harbour textures, got 72"),
    ("duplicate", "duplicate census texture"),
])
def test_census_scope_is_fixed_and_unique(dataset, change, message):
    source, rows, census_path, mapping_path, save = dataset
    if change == "missing":
        source.pop()
    elif change == "extra":
        source.append(dict(source[0], texture="NW_CITY_EXTRA.TGA"))
    else:
        source.append(deepcopy(source[0]))
    save()
    with pytest.raises(mapping.MappingError, match=message):
        mapping.check_mapping(mapping_path, census_path)


def test_market_wall_scope_admits_exactly_seven_known_misc_textures(dataset):
    source, rows, census_path, mapping_path, save = dataset
    for texture in sorted(mapping.MISC_TEXTURES):
        source.append(dict(source[0], texture=texture))
        rows.append(dict(rows[0], texture=texture))
    source.append(dict(source[0], texture="NW_MISC_STRONG_STONEWALL_HOLE_01.TGA"))
    save()
    assert len(mapping.check_mapping(mapping_path, census_path)) == 78
    source.pop(-2)
    save()
    with pytest.raises(mapping.MappingError, match="missing market/wall textures"):
        mapping.check_mapping(mapping_path, census_path)


def visual_counts(row):
    row.update(count_u="8", count_v="4", counted="stone blocks", count_direction="U",
        parts="", uv_m_u="4", uv_m_v="3", element_cm_u="50", element_cm_v="75")


@pytest.mark.parametrize("field,value,message", [
    ("count_u", "0", "count_u"), ("count_u", "NaN", "count_u"),
    ("count_v", "2.5", "count_v must be a whole number"),
    ("counted", "", "counted must be nonempty"),
    ("count_direction", "horizontal", "count_direction"),
    ("uv_m_u", "", "uv_m_u"), ("uv_m_v", "Infinity", "uv_m_v"),
    ("element_cm_u", "", "element_cm_u"),
    ("element_cm_v", "74", "stale element_cm_v"),
])
def test_visual_counts_validate_measured_sizes(dataset, field, value, message):
    _, rows, census_path, mapping_path, save = dataset
    visual_counts(rows[0])
    rows[0][field] = value
    save()
    with pytest.raises(mapping.MappingError, match=message):
        mapping.check_mapping(mapping_path, census_path)


def test_nonrepeating_axis_keeps_count_and_size_empty(dataset):
    _, rows, census_path, mapping_path, save = dataset
    visual_counts(rows[0])
    rows[0].update(count_u="", element_cm_u="")
    save()
    mapping.check_mapping(mapping_path, census_path)
    rows[0]["element_cm_u"] = "50"
    save()
    with pytest.raises(mapping.MappingError, match="empty count_u requires empty element_cm_u"):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("index", [-4, -3, -2])
def test_composite_visual_counts_require_parts(dataset, index):
    _, rows, census_path, mapping_path, save = dataset
    visual_counts(rows[index])
    save()
    with pytest.raises(mapping.MappingError, match="requires nonempty parts"):
        mapping.check_mapping(mapping_path, census_path)
    rows[index]["parts"] = "left and right posts; top and bottom rails"
    save()
    mapping.check_mapping(mapping_path, census_path)


def test_pilot_half_count_and_visual_measurements_survive_census_update(dataset):
    source, rows, census_path, mapping_path, save = dataset
    source[0]["texture"] = rows[0]["texture"] = "NW_HARBOUR_STAIRS_01.TGA"
    visual_counts(rows[0])
    rows[0].update(count_u="2.5", element_cm_u="160")
    before = {field: rows[0][field] for field in mapping.COUNT_FIELDS}
    source[0]["uv_m_per_unit_median"] = "6"
    save()
    updated = mapping.update_mapping(mapping_path, census_path)
    assert {field: updated[0][field] for field in mapping.COUNT_FIELDS} == before


@pytest.mark.parametrize("index,material", [(-1, ""), (-3, "iron_panel")])
def test_exclude_and_cannon_targets_keep_visual_count_fields_empty(dataset, index, material):
    _, rows, census_path, mapping_path, save = dataset
    visual_counts(rows[index])
    rows[index]["class"] = material
    save()
    with pytest.raises(mapping.MappingError, match="requires empty count fields"):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("field,value,message", [
    ("decision", "pending", "decision"),
    ("class", "stone", "unknown class"),
    ("class", "", "unknown class"),
    ("state", "worn", "2–5 English words"),
    ("state", "one two three four five six", "2–5 English words"),
    ("state", "очень старый", "2–5 English words"),
    ("state", "worn 2", "2–5 English words"),
    ("reason", " ", "reason must be nonempty"),
    ("area_m2", "NaN", "finite"),
    ("area_m2", "-1", "nonnegative"),
    ("uv_m_per_unit_median", "0", "positive"),
    ("scale", "Infinity", "finite"),
    ("scale", "0", "positive"),
    ("object", "", "object must contain nonempty English words"),
    ("role", "  ", "role must contain nonempty English words"),
    ("depicts", "штукатурка", "depicts must contain nonempty English words"),
    ("colour", "123", "colour must contain nonempty English words"),
    ("weathering", "dirt\ninside", "weathering must contain nonempty English words"),
    ("orientation", "", "orientation must contain nonempty English words"),
    ("layout", "repeated", "layout must be one of"),
    ("anti_repeat", "false", "anti_repeat must be on or off"),
])
def test_invalid_mapping_fields_have_diagnostics(dataset, field, value, message):
    source, rows, census_path, mapping_path, save = dataset
    rows[0][field] = value
    save()
    with pytest.raises(mapping.MappingError, match=message):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("field,value", [("class", "cobble"), ("state", "very worn"), ("scale", "2.51")])
def test_excluded_rows_have_no_generation_fields(dataset, field, value):
    source, rows, census_path, mapping_path, save = dataset
    rows[-1][field] = value
    save()
    with pytest.raises(mapping.MappingError, match=f"exclude requires empty {field}"):
        mapping.check_mapping(mapping_path, census_path)


def test_layout_mixture_replaces_the_old_triangle_uv_threshold(dataset):
    source, rows, census_path, mapping_path, save = dataset
    source[0]["tiled_area_fraction"] = "0.01"
    save()
    mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("value,message", [
    ("tiled:1;panel:0;strip:0", "contain each"),
    ("tiled:1;panel:0;strip:0;panel:0", "contain each"),
    ("tiled:1;panel:0;strip:0;other:0", "contain each"),
    ("tiled=1;panel:0;strip:0;atlas:0", "contain each"),
    ("tiled:0.5;panel:0.1;strip:0.1;atlas:0.1", "must sum to 1"),
    ("tiled:0.8;panel:0.2;strip:0;atlas:-0.1", "nonnegative"),
    ("tiled:NaN;panel:0;strip:0;atlas:0", "finite"),
    ("tiled:1.1;panel:0;strip:0;atlas:0", "at most 1"),
    ("tiled:0.4;panel:0.6;strip:0;atlas:0", "largest layout_share"),
])
def test_invalid_layout_mixtures_have_diagnostics(dataset, value, message):
    source, rows, census_path, mapping_path, save = dataset
    rows[0]["layout_share"] = value
    save()
    with pytest.raises(mapping.MappingError, match=message):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("value", [
    "tiled:0.5;panel:0.5;strip:0;atlas:0",
    "tiled:0.8;panel:0.1;strip:0.1;atlas:0.00001",
    "tiled:0.8;panel:0.1;strip:0.09999;atlas:0",
])
def test_layout_accepts_ties_and_rounding_tolerance(dataset, value):
    source, rows, census_path, mapping_path, save = dataset
    rows[0]["layout_share"] = value
    save()
    mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("index,decision", [(0, "panel"), (-3, "include"), (-2, "panel"), (-1, "strip")])
def test_layout_determines_decision(dataset, index, decision):
    source, rows, census_path, mapping_path, save = dataset
    rows[index]["decision"] = decision
    save()
    with pytest.raises(mapping.MappingError, match="layout requires decision"):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("index", [0, -3, -2])
def test_exclusion_preserves_measured_layout_for_unsupported_surfaces(dataset, index):
    source, rows, census_path, mapping_path, save = dataset
    rows[index].update(decision="exclude", **{
        "class": "", "state": "", "scale": "", "reason": "unsupported painted surface", "anti_repeat": "off",
    })
    save()
    assert mapping.check_mapping(mapping_path, census_path)[index] == rows[index]


@pytest.mark.parametrize("index,field,value,message", [
    (-3, "class", "plaster", "unknown class"),
    (-2, "class", "timber_beam", "unknown class"),
    (-3, "scale", "2.51", "stale scale"),
    (-2, "scale", "2.51", "stale scale"),
    (-3, "state", "worn", "2–5 English words"),
    (-4, "class", "timber_beam", "unknown class"),
    (-4, "scale", "2.51", "stale scale"),
    (-4, "state", "worn", "2–5 English words"),
])
def test_fixed_layout_decisions_validate_their_own_generation_contract(dataset, index, field, value, message):
    source, rows, census_path, mapping_path, save = dataset
    rows[index][field] = value
    save()
    with pytest.raises(mapping.MappingError, match=message):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("layout,shares", [
    ("tiled", "tiled:1;panel:0;strip:0;atlas:0"),
    ("panel", "tiled:0;panel:1;strip:0;atlas:0"),
    ("strip", "tiled:0;panel:0;strip:1;atlas:0"),
])
def test_module_accepts_each_nonatlas_geometry_layout(dataset, layout, shares):
    source, rows, census_path, mapping_path, save = dataset
    rows[-4].update(layout=layout, layout_share=shares)
    save()
    mapping.check_mapping(mapping_path, census_path)


def test_module_cannot_replace_an_atlas(dataset):
    source, rows, census_path, mapping_path, save = dataset
    rows[-4].update(layout="atlas", layout_share="tiled:0;panel:0;strip:0;atlas:1")
    save()
    with pytest.raises(mapping.MappingError, match="atlas layout requires decision exclude"):
        mapping.check_mapping(mapping_path, census_path)


def test_directional_tiled_surface_can_disable_anti_repeat(dataset):
    source, rows, census_path, mapping_path, save = dataset
    rows[0].update(**{
        "class": "planks_floor", "scale": "2.51", "anti_repeat": "off",
        "object": "tavern floor", "role": "walking surface", "depicts": "parallel wooden boards",
        "colour": "warm brown", "weathering": "worn board faces", "orientation": "grain runs along boards",
    })
    save()
    assert mapping.check_mapping(mapping_path, census_path)[0]["anti_repeat"] == "off"


@pytest.mark.parametrize("value", ["", "NaN", "Infinity", "-0.1", "1.01", "unknown"])
def test_pilot_requires_finite_unit_interval_target_luminance(dataset, value):
    _, rows, census_path, mapping_path, save = dataset
    rows[0]["target_luminance"] = value
    save()
    with pytest.raises(mapping.MappingError, match="target_luminance"):
        mapping.check_mapping(mapping_path, census_path)


def test_facade_requires_target_but_other_modules_may_leave_it_empty(dataset):
    source, rows, census_path, mapping_path, save = dataset
    rows[-4]["target_luminance"] = ""
    save()
    mapping.check_mapping(mapping_path, census_path)
    source[-4]["texture"] = rows[-4]["texture"] = "NW_CITY_WALL_03.TGA"
    save()
    with pytest.raises(mapping.MappingError, match="target_luminance"):
        mapping.check_mapping(mapping_path, census_path)


def test_mapping_update_preserves_target_luminance(dataset):
    _, rows, census_path, mapping_path, save = dataset
    targets = [row["target_luminance"] for row in rows]
    rows[0]["scale"] = "9.99"
    save()
    assert [row["target_luminance"] for row in mapping.update_mapping(mapping_path, census_path)] == targets


@pytest.mark.parametrize("value", ["NaN", "Infinity", "-0.1", "1.01", "unknown"])
def test_optional_coverage_requires_a_finite_unit_interval_scalar(dataset, value):
    _, rows, census_path, mapping_path, save = dataset
    rows[-4]["target_coverage"] = value
    save()
    with pytest.raises(mapping.MappingError, match="target_coverage"):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("texture", sorted(mapping.ALPHA_TEXTURES))
def test_alpha_requires_coverage_and_census_update_preserves_it(dataset, texture):
    source, rows, census_path, mapping_path, save = dataset
    source[-4]["texture"] = rows[-4]["texture"] = texture
    save()
    with pytest.raises(mapping.MappingError, match="target_coverage"):
        mapping.check_mapping(mapping_path, census_path)
    rows[-4]["target_coverage"] = "0.500400543213"
    rows[-4]["scale"] = "9"
    save()
    updated = mapping.update_mapping(mapping_path, census_path)
    assert updated[-4]["target_coverage"] == "0.500400543213"
    assert updated[-4]["scale"] == "1.00"


def test_legacy_header_without_coverage_remains_readable(dataset):
    _, rows, census_path, mapping_path, _ = dataset
    legacy = [{field: row[field] for field in mapping.FIELDS[:-1]} for row in rows]
    write_csv(mapping_path, mapping.FIELDS[:-1], legacy)
    assert mapping.check_mapping(mapping_path, census_path) == rows


@pytest.mark.parametrize("header", [mapping.MEASURED_FIELDS, mapping.MEASURED_FIELDS[:-1]])
def test_legacy_header_without_visual_counts_remains_readable(dataset, header):
    _, rows, census_path, mapping_path, _ = dataset
    legacy = [{field: row[field] for field in header} for row in rows]
    write_csv(mapping_path, header, legacy)
    assert mapping.check_mapping(mapping_path, census_path) == rows


@pytest.mark.parametrize("header", [header for header in mapping.MAPPING_HEADERS
                                   if not set(mapping.DETAIL_FIELDS) <= set(header)])
def test_previous_headers_read_but_check_requires_detail_targets(dataset, header):
    _, rows, census_path, mapping_path, _ = dataset
    legacy = [{field: row[field] for field in header} for row in rows]
    write_csv(mapping_path, header, legacy)
    loaded = mapping._read_csv(mapping_path, mapping=True)
    assert [{field: row[field] for field in header} for row in loaded] == legacy
    assert all(row[field] == "" for row in loaded for field in mapping.DETAIL_FIELDS)
    missing_field = "target_log_spread" if "target_luminance" in header else "target_luminance"
    with pytest.raises(mapping.MappingError, match=missing_field):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("index", [-4, -3, -2, -1])
def test_noninclude_decisions_require_anti_repeat_off(dataset, index):
    source, rows, census_path, mapping_path, save = dataset
    rows[index]["anti_repeat"] = "on"
    save()
    with pytest.raises(mapping.MappingError, match="requires anti_repeat=off"):
        mapping.check_mapping(mapping_path, census_path)


@pytest.mark.parametrize("field,value,message", [
    ("area_m2", "bad", "finite number"),
    ("area_m2", "-1", "nonnegative"),
    ("uv_m_per_unit_median", "0", "positive"),
    ("uv_m_per_unit_median", "NaN", "finite"),
    ("tiled_area_fraction", "Infinity", "finite"),
    ("tiled_area_fraction", "1.1", "at most 1"),
])
def test_invalid_census_measurements_are_rejected(dataset, field, value, message):
    source, rows, census_path, mapping_path, save = dataset
    source[0][field] = value
    save()
    with pytest.raises(mapping.MappingError, match=message):
        mapping.check_mapping(mapping_path, census_path)


def test_update_recalculates_source_numbers_and_scale_preserving_decisions(dataset):
    source, rows, census_path, mapping_path, save = dataset
    source[0].update(area_m2="20.125", uv_m_per_unit_median="6.015")
    source[-1].update(area_m2="0", uv_m_per_unit_median="1.23")
    rows[0]["class"] = "flagstone"
    rows[-4]["scale"] = "5"
    rows[-3]["scale"] = "2.5"
    rows[-2]["scale"] = "0.5"
    manual_fields = ("texture", "decision", "class", "state", "reason", *mapping.SEMANTIC_FIELDS, "layout", "layout_share", "anti_repeat")
    manual = [{field: row[field] for field in manual_fields} for row in rows]
    save()
    updated = mapping.update_mapping(mapping_path, census_path)
    assert updated[0]["scale"] == "2.01"
    assert updated[0]["area_m2"] == "20.125"
    assert updated[0]["uv_m_per_unit_median"] == "6.015"
    assert updated[-1]["area_m2"] == "0"
    assert updated[-1]["uv_m_per_unit_median"] == "1.23"
    assert updated[-1]["scale"] == ""
    assert updated[-4]["scale"] == updated[-3]["scale"] == updated[-2]["scale"] == "1.00"
    assert [{field: row[field] for field in manual[0]} for row in updated] == manual
    assert mapping.check_mapping(mapping_path, census_path) == updated


def test_invalid_update_does_not_write_partial_results(dataset):
    source, rows, census_path, mapping_path, save = dataset
    rows[0]["area_m2"] = "old"
    rows[-1]["reason"] = ""
    save()
    before = mapping_path.read_bytes()
    with pytest.raises(mapping.MappingError, match="reason must be nonempty"):
        mapping.update_mapping(mapping_path, census_path)
    assert mapping_path.read_bytes() == before


@pytest.mark.parametrize("text,message", [
    ("", "empty CSV"),
    ("texture,texture\n", "duplicate CSV column"),
    ("texture,decision\n", "expected CSV columns in order"),
    (",".join(mapping.FIELDS) + "\nshort,row\n", f"expected {len(mapping.FIELDS)} CSV fields, got 2"),
])
def test_malformed_mapping_csv(dataset, text, message):
    source, rows, census_path, mapping_path, save = dataset
    mapping_path.write_text(text)
    with pytest.raises(mapping.MappingError, match=message):
        mapping.check_mapping(mapping_path, census_path)


def test_cli_failures_are_actionable_without_traceback(tmp_path):
    missing = tmp_path / "missing.csv"
    result = subprocess.run([
        sys.executable, str(SCRIPT), "check", "--mapping", str(missing), "--census", str(missing),
    ], capture_output=True, text=True)
    assert result.returncode == 1
    assert "Material mapping check failed:" in result.stderr
    assert "missing.csv" in result.stderr
    assert "Traceback" not in result.stderr
