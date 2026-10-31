"""City-3 keeps non-metal city materials in one image_gen staging set."""
from copy import deepcopy
import csv
import json

import numpy as np
from PIL import Image
import pytest

import material_preview as preview
import pack_materials as packer
from pilot import CITY_3_DECISIONS, CITY_3_EXCLUDED_CLASSES, selected_rows


def test_city3_scope_has_58_unique_materials_and_preserves_csv_order():
    with packer.DEFAULT_MAPPING.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    selected = selected_rows(rows, "city-3")
    assert len(selected) == len({row["texture"] for row in selected}) == 58
    assert selected == [row for row in rows if row["decision"] in CITY_3_DECISIONS
                        and not row["texture"].startswith("NW_MISC_")
                        and row["class"] not in CITY_3_EXCLUDED_CLASSES]
    assert {row["decision"] for row in selected} == CITY_3_DECISIONS
    assert not {row["class"] for row in selected} & CITY_3_EXCLUDED_CLASSES


def test_city3_skips_only_standalone_metal_for_all_decisions():
    rows = [{"texture": f"{index}.TGA", "decision": decision, "class": name}
            for index, (decision, name) in enumerate(
                (decision, name) for decision in CITY_3_DECISIONS
                for name in CITY_3_EXCLUDED_CLASSES)]
    rows += [{"texture": "WALL.TGA", "decision": "module", "class": "framed_masonry_tile"},
             {"texture": "EXCLUDED.TGA", "decision": "exclude", "class": "stone_panel"}]
    assert selected_rows(rows, "city-3") == rows[-2:-1]


def test_city3_includes_cloth_ivy_and_rope():
    rows = [{"texture": name + ".TGA", "decision": decision, "class": name}
            for name, decision in (("cloth_panel", "panel"), ("foliage_panel", "panel"),
                                   ("rope_strip", "strip"))]
    assert selected_rows(rows, "city-3") == rows


@pytest.fixture
def city3_set(tmp_path):
    rows = [
        {"texture": "FLOOR.TGA", "decision": "include", "class": "flagstone",
         "scale": "2.00", "target_luminance": "0.15"},
        {"texture": "WALL.TGA", "decision": "module", "class": "framed_masonry_tile",
         "scale": "1.00", "target_luminance": "0.21"},
        {"texture": "PANEL.TGA", "decision": "panel", "class": "painted_panel",
         "scale": "1.00", "target_luminance": "0.19"},
        {"texture": "STRIP.TGA", "decision": "strip", "class": "stone_strip",
         "scale": "1.00", "target_luminance": "0.11"},
    ]
    mapping = tmp_path / "mapping.csv"
    with mapping.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
        writer.writerow({"texture": "IRON.TGA", "decision": "panel", "class": "iron_panel",
                         "scale": "1", "target_luminance": ""})
    root = tmp_path / "sources"
    records = []
    for index, row in enumerate(rows):
        resource_id = f"city-3-material-{index}"
        directory = root / resource_id
        directory.mkdir(parents=True)
        for channel in packer.MAPS:
            if channel == "normal":
                value = np.full((16, 16, 3), (128, 128, 255), dtype=np.uint8)
            elif channel == "albedo":
                value = np.full((16, 16, 3), (90, 74, 56), dtype=np.uint8)
            else:
                value = np.full((16, 16), {"ao": 255, "roughness": 210, "metallic": 0}[channel], dtype=np.uint8)
            Image.fromarray(value).save(directory / f"{channel}.png")
        records.append({
            "id": resource_id, "kind": "material", "pipeline": "imagegen-filter", "status": "draft",
            "parameters": {"material": resource_id, "material_class": row["class"], "decision": row["decision"],
                           "set": "city-3", "scale": float(row["scale"]),
                           "target_luminance": float(row["target_luminance"]),
                           "antitile": packer.antitile_mode(row), "antitile_cell": 1.0,
                           "source_aspect": 1.5, "tiling_axes": "uv" if row["decision"] == "module" else ""},
            "replaces": [row["texture"]], "generation": {"outcome": "complete"},
            "files": {channel: f"{resource_id}/{channel}.png" for channel in packer.MAPS},
        })
    return root, records, mapping


def test_city3_packs_imagegen_with_mapped_targets_and_placement(city3_set, tmp_path):
    root, records, mapping = city3_set
    rejected = deepcopy(records[0])
    rejected["parameters"]["selected"] = False
    selected = packer.select_set_records(records + [rejected], "city-3", mapping)
    assert selected == records
    output = packer.pack_set(selected, root, tmp_path / "city-3")
    index = json.loads((output / "materials/materials.json").read_text())
    assert len(index) == 4
    assert len(list(output.rglob("*.dds"))) == 12
    assert index["FLOOR.TGA"]["scale"] == 2
    assert index["FLOOR.TGA"]["antitile"] == "rotate"
    for texture in ("WALL.TGA", "PANEL.TGA", "STRIP.TGA"):
        assert index[texture]["scale"] == 1
        assert index[texture]["antitile"] == "off"
    assert all(entry["antitile_cell"] == 1 for entry in index.values())


@pytest.mark.parametrize("pipeline", ["flux-filter", "flux-marigold", "sdxl-filter"])
def test_city3_rejects_other_generators(city3_set, pipeline):
    _, records, mapping = city3_set
    records[0]["pipeline"] = pipeline
    with pytest.raises(ValueError, match="must be a draft from imagegen-filter"):
        packer.select_set_records(records, "city-3", mapping)


@pytest.mark.parametrize("field,value,message", [
    ("target_luminance", None, "target_luminance must be finite"),
    ("target_luminance", 0.2, "target_luminance differs"),
    ("antitile", "off", "antitile differs"),
    ("antitile_cell", 0, "antitile_cell must be finite"),
    ("scale", 1, "scale differs"),
])
def test_city3_rejects_target_or_placement_drift(city3_set, field, value, message):
    _, records, mapping = city3_set
    records[0]["parameters"][field] = value
    with pytest.raises(ValueError, match=message):
        packer.select_set_records(records, "city-3", mapping)


def test_city3_modules_require_one_repeat_even_if_mapping_scale_is_wrong(city3_set):
    _, records, mapping = city3_set
    records[1]["parameters"]["scale"] = 2
    mapping.write_text(mapping.read_text().replace("WALL.TGA,module,framed_masonry_tile,1.00",
                                                  "WALL.TGA,module,framed_masonry_tile,2.00"))
    with pytest.raises(ValueError, match="architectural modules require scale=1"):
        packer.select_set_records(records, "city-3", mapping)


def test_set_generator_labels_reflect_selected_pipeline():
    assert preview.set_generator_label([{"pipeline": "imagegen-filter"}]) == "image_gen + generated height filter"
    assert preview.set_generator_label([{"pipeline": "flux-filter"}]) == "FLUX.1-schnell + generated height filter"
    assert preview.set_generator_label([{"pipeline": "imagegen-filter"}, {"pipeline": "flux-filter"}]) == (
        "FLUX.1-schnell + image_gen + generated height filter")
