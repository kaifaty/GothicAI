"""Full staging sets preserve the CSV mapping without enabling live content."""

from copy import deepcopy
import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
from PIL import Image
import pytest


DIRECTORY = Path(__file__).parent


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, DIRECTORY / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


packer = load_module("pack_materials")
preview = load_module("material_preview")


def fixture_records(tmp_path, rows):
    root = tmp_path / "source"
    root.mkdir()
    records = []
    for row in rows:
        resource_id = "city-1-" + Path(row["texture"]).stem.lower().replace("_", "-")
        directory = root / resource_id
        directory.mkdir()
        for channel in packer.MAPS:
            if channel == "normal":
                value = np.full((16, 16, 3), (128, 128, 255), dtype=np.uint8)
            elif channel == "albedo":
                value = np.full((16, 16, 3), (90, 74, 56), dtype=np.uint8)
                value[:8, :8] = (120, 100, 80)
            else:
                value = np.full((16, 16), {"ao": 255, "roughness": 210, "metallic": 0}[channel], dtype=np.uint8)
            Image.fromarray(value).save(directory / f"{channel}.png")
        records.append({
            "id": resource_id, "kind": "material", "pipeline": "flux-filter", "status": "draft",
            "parameters": {"material": resource_id, "material_class": row["class"],
                           "set": "city-1", "scale": float(row["scale"])},
            "replaces": [row["texture"]], "generation": {"outcome": "complete"},
            "files": {channel: f"{resource_id}/{channel}.png" for channel in packer.MAPS},
        })
    mapping = tmp_path / "mapping.csv"
    with mapping.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=("texture", "decision", "class", "scale"))
        writer.writeheader()
        writer.writerows({key: row[key] for key in writer.fieldnames} for row in rows)
        writer.writerow({"texture": "EXCLUDED.TGA", "decision": "exclude", "class": "", "scale": ""})
    return root, records, mapping


@pytest.fixture
def small_set(tmp_path):
    return fixture_records(tmp_path, [
        {"texture": "NW_CITY_WALL_03.TGA", "decision": "include", "class": "plaster", "scale": "1.24"},
        {"texture": "NW_CITY_HAFENKAI_BODEN_01.TGA", "decision": "include", "class": "flagstone", "scale": "2.00"},
    ])


def test_entire_city_table_packs_to_one_root_with_csv_scales(tmp_path):
    with packer.DEFAULT_MAPPING.open() as stream:
        rows = [row for row in csv.DictReader(stream) if row["decision"] == "include"
                and not row["texture"].startswith("NW_MISC_")]
    root, records, mapping = fixture_records(tmp_path, rows)
    selected = packer.select_set_records(records, "city-1", mapping)
    output = packer.pack_set(selected, root, tmp_path / "content-staging/city-1")
    index = json.loads((output / "materials/materials.json").read_text())
    assert len(index) == len(rows)
    assert set(index) == {row["texture"] for row in rows}
    assert len(list(output.rglob("materials.json"))) == 1
    assert len(list(output.rglob("*.dds"))) == len(rows) * 3
    for row in rows:
        entry = index[row["texture"]]
        assert entry["scale"] == float(row["scale"])
        for channel in ("albedo", "normal", "orm"):
            data = (output / "materials" / entry[channel]).read_bytes()
            assert data[:4] == b"DDS "
            assert data[84:88] == (b"DXT5" if channel == "normal" else b"DXT1")


def test_selection_ignores_other_sets_pending_and_rejected_attempts(small_set):
    _, records, mapping = small_set
    unused = deepcopy(records[0])
    unused["parameters"]["set"] = "other-set"
    pending = deepcopy(records[0])
    pending["generation"]["outcome"] = "pending"
    pending["files"] = {}
    rejected = deepcopy(records[0])
    rejected["parameters"]["selected"] = False
    assert packer.select_set_records(records + [unused, pending, rejected], "city-1", mapping) == records


def test_selection_rejects_missing_duplicate_and_excluded_textures(small_set):
    _, records, mapping = small_set
    with pytest.raises(ValueError, match="missing completed"):
        packer.select_set_records(records[:1], "city-1", mapping)
    with pytest.raises(ValueError, match="duplicate selected"):
        packer.select_set_records(records + [deepcopy(records[0])], "city-1", mapping)
    records[0]["replaces"] = ["EXCLUDED.TGA"]
    with pytest.raises(ValueError, match="not an include texture"):
        packer.select_set_records(records, "city-1", mapping)


@pytest.mark.parametrize("field,value,message", [
    ("scale", 1.0, "scale differs"), ("scale", float("nan"), "finite and positive"),
    ("scale", True, "finite and positive"), ("material_class", "cobble", "material_class differs"),
])
def test_selection_requires_csv_class_and_scale(small_set, field, value, message):
    _, records, mapping = small_set
    records[0]["parameters"][field] = value
    with pytest.raises(ValueError, match=message):
        packer.select_set_records(records, "city-1", mapping)


def test_live_output_and_symlink_alias_are_refused(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    live = Path.home() / ".local/share/gothic-ai/content"
    live.mkdir(parents=True)
    alias = tmp_path / "alias"
    alias.symlink_to(live, target_is_directory=True)
    for path in (live, live / "city-1", alias / "city-1"):
        with pytest.raises(ValueError, match="live"):
            packer.staging_output(path)
    assert list(live.iterdir()) == []


def test_pack_set_refuses_existing_output_symlink(small_set, tmp_path):
    root, records, _ = small_set
    output = tmp_path / "staging"
    (output / "materials").mkdir(parents=True)
    outside = tmp_path / "outside"
    outside.mkdir()
    (output / "materials" / records[0]["id"]).symlink_to(outside, target_is_directory=True)
    with pytest.raises(ValueError, match="symlink escapes"):
        packer.pack_set(records, root, output)
    assert list(outside.iterdir()) == []


def test_incomplete_cli_does_not_create_output(small_set, tmp_path):
    root, records, mapping = small_set
    registry = tmp_path / "registry.jsonl"
    registry.write_text(json.dumps(records[0]) + "\n")
    output = tmp_path / "staging"
    result = subprocess.run([sys.executable, str(DIRECTORY / "pack_materials.py"),
                             "--registry", str(registry), "--content-root", str(root),
                             "--set", "city-1", "--mapping", str(mapping), "--output", str(output)],
                            capture_output=True, text=True)
    assert result.returncode == 1
    assert "missing completed" in result.stderr
    assert not output.exists()


def test_set_sheet_has_each_material_and_repeated_albedo(small_set, tmp_path):
    root, records, mapping = small_set
    registry = tmp_path / "registry.jsonl"
    registry.write_text("".join(json.dumps(record) + "\n" for record in records))
    output = tmp_path / "report"
    result = subprocess.run([sys.executable, str(DIRECTORY / "material_preview.py"),
                             "--registry", str(registry), "--content-root", str(root),
                             "--set", "city-1", "--mapping", str(mapping), "--output", str(output),
                             "--cell-size", "128"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert "2 material previews" in result.stdout
    with Image.open(output / "materials.jpg") as sheet:
        assert sheet.format == "JPEG"
        assert sheet.size == (862, 316)
        # First panel repeats one full tile in each quadrant.
        array = np.asarray(sheet)
        a = array[160:200, 26:66].astype(np.int16)
        b = array[224:264, 90:130].astype(np.int16)
        assert np.abs(a - b).mean() < 2


def test_bakeoff_pack_keeps_separate_single_texture_roots(small_set, tmp_path):
    root, records, _ = small_set
    record = records[0]
    record["parameters"]["material"] = "stone-wall"
    output = packer.pack(record, root, tmp_path / "bakeoff", 1.0)
    index = json.loads((output / "materials/materials.json").read_text())
    assert set(index) == {"NW_CITY_WALL_03.TGA"}
    assert index["NW_CITY_WALL_03.TGA"]["scale"] == 1.0


@pytest.fixture
def city2_set(tmp_path):
    rows = [
        {"texture": "NW_CITY_HAFENKAI_BODEN_01.TGA", "decision": "include", "class": "flagstone",
         "scale": "2.00", "target_luminance": "0.15", "anti_repeat": "on"},
        {"texture": "NW_CITY_WALL_03.TGA", "decision": "module", "class": "timber_frame_tile",
         "scale": "1.00", "target_luminance": "0.21", "anti_repeat": "off"},
    ]
    root, records, mapping = fixture_records(tmp_path, rows)
    for record, row in zip(records, rows):
        record["parameters"].update(set="city-2", target_luminance=float(row["target_luminance"]),
                                    antitile=packer.antitile_mode(row), antitile_cell=1.0)
    with mapping.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
        writer.writerow({"texture": "OTHER_MODULE.TGA", "decision": "module", "class": "window_wall_tile",
                         "scale": "1", "anti_repeat": "off", "target_luminance": ""})
    return root, records, mapping


def test_city2_packs_include_and_facade_with_recorded_antitile(city2_set, tmp_path):
    root, records, mapping = city2_set
    records[1]["generation"].update(selected_failed=True, failure_warning="best of three attempts")
    selected = packer.select_set_records(records, "city-2", mapping)
    output = packer.pack_set(selected, root, tmp_path / "city-2")
    index = json.loads((output / "materials/materials.json").read_text())
    assert len(index) == 2
    assert index["NW_CITY_HAFENKAI_BODEN_01.TGA"]["antitile"] == "rotate"
    assert index["NW_CITY_WALL_03.TGA"]["antitile"] == "off"
    assert index["NW_CITY_WALL_03.TGA"]["scale"] == 1
    assert all(entry["antitile_cell"] == 1 for entry in index.values())


def test_imagegen_set_packs_mixed_pipelines_with_city2_placement(city2_set, tmp_path):
    root, records, mapping = city2_set
    for record in records:
        record["parameters"]["set"] = "city-2-imagegen"
    records[1]["pipeline"] = "imagegen-filter"
    selected = packer.select_set_records(records, "city-2-imagegen", mapping)
    output = packer.pack_set(selected, root, tmp_path / "city-2-imagegen")
    index = json.loads((output / "materials/materials.json").read_text())
    assert set(index) == {record["replaces"][0] for record in records}
    for record in records:
        entry = index[record["replaces"][0]]
        assert entry["scale"] == record["parameters"]["scale"]
        assert entry["antitile"] == record["parameters"]["antitile"]
        assert entry["antitile_cell"] == record["parameters"]["antitile_cell"]
    # The A/B generator is not accepted into the immutable city-2 set.
    for record in records:
        record["parameters"]["set"] = "city-2"
    with pytest.raises(ValueError, match="must be a draft from flux-filter"):
        packer.select_set_records(records, "city-2", mapping)


@pytest.mark.parametrize("set_name", ["city-2", "city-2-imagegen"])
@pytest.mark.parametrize("field,value,message", [
    ("target_luminance", 0.19, "target_luminance differs"),
    ("target_luminance", float("nan"), "finite and in"),
    ("target_luminance", True, "finite and in"),
    ("antitile", "off", "antitile differs"),
    ("antitile_cell", 0, "antitile_cell must be finite and positive"),
])
def test_city2_selection_validates_scalar_target_and_antitile(city2_set, set_name, field, value, message):
    _, records, mapping = city2_set
    for record in records:
        record["parameters"]["set"] = set_name
    records[0]["parameters"][field] = value
    with pytest.raises(ValueError, match=message):
        packer.select_set_records(records, set_name, mapping)
