"""City-4 tour coverage and census requirements, without a game or original pixels."""
import importlib.util
import json
from pathlib import Path
import re
import subprocess

from PIL import Image
import pytest


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("capture_city4", HERE / "capture-city-4.py")
CAPTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAPTURE)


def prepared_tour(tmp_path):
    names = re.findall(r"^# ([A-Z0-9_]+\.TGA) \|", (HERE / "city-intent.bench").read_text(), re.MULTILINE)[:58]
    misc = [f"NW_MISC_TEST_{number}.TGA" for number in range(7)]
    index = tmp_path / "materials.json"
    index.write_text(json.dumps({name: {"albedo": f"tile-{i}/albedo.dds"}
                                 for i, name in enumerate(names + misc)}))
    inspection = tmp_path / "misc.bench"
    inspection.write_text("\n".join(
        f"# {name} | MARKT | synthetic test camera\ncam look 45\ngoto pos 1 2 3\n"
        f"cam world 4 5 6\nwait 34\ncam hold\nwait 600\nshot {Path(name).stem.lower()}\nwait 300\n"
        for name in misc))
    scenario = tmp_path / "city-4.bench"
    CAPTURE.prepare(index, scenario, inspection)
    return index, scenario, inspection


def test_tour_contains_all_comparisons_and_a_census_camera_for_every_target(tmp_path):
    index, scenario, _ = prepared_tour(tmp_path)
    text = scenario.read_text()
    shots = [line[5:] for line in CAPTURE.commands(scenario) if line.startswith("shot ")]
    assert len(shots) == len(set(shots)) == 35
    comparisons = set(shots) - {"city_a", "city_night"}
    assert len(comparisons) == 33
    assert {"pilot_" + name for name in ("roof", "upper", "house", "interior", "facade", "quay", "stairs")} <= comparisons
    assert len([name for name in comparisons if name.startswith("nw_misc_")]) == 7
    tail = text.split("# Census-only surface tour:", 1)[1]
    assert tail.count("cam hold") == len(json.loads(index.read_text())) == 65
    assert "shot " not in tail


def test_missing_misc_camera_stops_before_writing_a_tour(tmp_path):
    index, scenario, inspection = prepared_tour(tmp_path)
    scenario.unlink()
    inspection.write_text(inspection.read_text().split("# NW_MISC_TEST_6.TGA", 1)[0])
    with pytest.raises(SystemExit, match="seven NW_MISC"):
        CAPTURE.prepare(index, scenario, inspection)
    assert not scenario.exists()


def census_capture(tmp_path, variant="city-4"):
    directory = tmp_path / variant
    directory.mkdir()
    scenario = tmp_path / "capture.bench"
    scenario.write_text("goto MARKT\nshot market\n")
    (directory / "market.png").write_bytes(b"synthetic screenshot")
    (directory / "exit.txt").write_text("0\n")
    count = {"city-4": 65, "city-3": 58, "original": 0}[variant]
    (directory / "game.log").write_text(f"materials: {count} entries from fixture\n"
                                       "[bench] goto MARKT\n[bench] shot market\n[bench] done\n")
    names = [f"TEST_{i}.TGA" for i in range(58 if variant == "city-3" else 65)]
    if variant == "city-3":
        scenario.write_text(scenario.read_text() + "# Census-only surface tour:\n"
                            + "\n".join("# " + name for name in names)
                            + "\n# NW_MISC_TEST_0.TGA\n")
    index = tmp_path / "materials.json"
    index.write_text(json.dumps({name: {"albedo": f"tile-{i}/albedo.dds"} for i, name in enumerate(names)}))
    originals = [{"name": name[:-4] + "-C.TEX", "kind": "texture", "opens": 1, "source": "fixture.vdf"}
                 for name in names]
    replacements = [{"name": name, "kind": "texture", "opens": 1, "replacement": f"materials/tile-{i}/albedo.dds"}
                    for i, name in enumerate(names)]
    census = {"files": originals if variant == "original" else [],
              "replacements": [] if variant == "original" else replacements}
    (directory / "census.json").write_text(json.dumps(census))
    return directory, scenario, index, census, originals


@pytest.mark.parametrize("variant", ("original", "city-3", "city-4"))
def test_capture_accepts_required_variant_counts(tmp_path, variant):
    directory, scenario, index, _, _ = census_capture(tmp_path, variant)
    CAPTURE.check(directory, scenario, index, variant)


@pytest.mark.parametrize("variant", ("city-3", "city-4"))
@pytest.mark.parametrize("failure", ("missing_replacement", "original_alias", "no_replacements",
                                    "wrong_albedo_path", "missing_census"))
def test_replacement_variants_require_complete_census(tmp_path, variant, failure):
    directory, scenario, index, census, originals = census_capture(tmp_path, variant)
    if failure == "missing_replacement":
        census["replacements"].pop()
    elif failure == "original_alias":
        census["files"].append(originals[0])
    elif failure == "no_replacements":
        census["replacements"] = []
    elif failure == "wrong_albedo_path":
        census["replacements"][0]["replacement"] = "materials/unrelated/albedo.dds"
    (directory / "census.json").write_text(json.dumps(census))
    if failure == "missing_census":
        (directory / "census.json").unlink()
    with pytest.raises(subprocess.CalledProcessError):
        CAPTURE.check(directory, scenario, index, variant)


@pytest.mark.parametrize("replaced", (False, True))
def test_city3_rejects_58_unrelated_materials(tmp_path, replaced):
    directory, scenario, index, census, _ = census_capture(tmp_path, "city-3")
    names = [f"UNRELATED_{i}.TGA" for i in range(58)]
    index.write_text(json.dumps({name: {"albedo": f"unrelated-{i}/albedo.dds"}
                                 for i, name in enumerate(names)}))
    census["files"] = [{"name": "NW_CITY_ROOF_01-C.TEX", "kind": "texture", "opens": 1,
                        "source": "fixture.vdf"}]
    census["replacements"] = ([{"name": name, "kind": "texture", "opens": 1,
                                "replacement": f"materials/unrelated-{i}/albedo.dds"}
                               for i, name in enumerate(names)] if replaced else [])
    (directory / "census.json").write_text(json.dumps(census))
    with pytest.raises(SystemExit, match="58 city census targets"):
        CAPTURE.check(directory, scenario, index, "city-3")


def test_city3_requires_census_target_list_in_scenario(tmp_path):
    directory, scenario, index, _, _ = census_capture(tmp_path, "city-3")
    scenario.write_text("goto MARKT\nshot market\n")
    with pytest.raises(SystemExit, match="58 city census targets"):
        CAPTURE.check(directory, scenario, index, "city-3")


def test_original_baseline_rejects_even_an_unrelated_replacement(tmp_path):
    directory, scenario, index, census, _ = census_capture(tmp_path, "original")
    census["replacements"].append({"name": "UNRELATED.TGA", "kind": "texture", "opens": 1,
                                   "replacement": "materials/unrelated.dds"})
    (directory / "census.json").write_text(json.dumps(census))
    with pytest.raises(SystemExit, match="zero replacements"):
        CAPTURE.check(directory, scenario, index, "original")


def test_compose_writes_one_jpeg_triple_per_comparison_point(tmp_path):
    _, scenario, _ = prepared_tour(tmp_path)
    captures = tmp_path / "captures"
    for variant in CAPTURE.VARIANTS:
        (captures / variant).mkdir(parents=True)
        for command in CAPTURE.commands(scenario):
            if command.startswith("shot "):
                Image.new("RGB", (20, 10), "#ab8050").save(captures / variant / (command[5:] + ".png"))
    output = tmp_path / "triples"
    CAPTURE.compose(captures, scenario, output)
    assert len(list(output.glob("*.jpg"))) == 33
    with Image.open(output / "pilot_roof.jpg") as image:
        assert image.format == "JPEG" and image.size == (60, 54)
