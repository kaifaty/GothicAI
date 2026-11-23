"""Full-frame pilot statistics and their per-spot baseline ratios."""

import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
from PIL import Image
import pytest


SPEC = importlib.util.spec_from_file_location(
    "capture_quality_pilot", Path(__file__).with_name("capture-quality-pilot.py"))
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


def capture_check_inputs(tmp_path, variant, materials, *, original=False, market_house=False):
    directory = tmp_path / variant
    directory.mkdir()
    (directory / "exit.txt").write_text("0\n")
    scenario = tmp_path / "pilot.bench"
    spots = capture.HOUSE_SPOTS if market_house else capture.SPOTS
    scenario.write_text("".join(f"shot {spot}\n" for spot in spots))
    index = tmp_path / "materials.json"
    material_count = 58 if market_house else 1
    index.write_text(json.dumps({} if original else {f"test-{i}": {} for i in range(material_count)}))
    (directory / "game.log").write_text(
        f"materials: {0 if original else material_count} entries from test\n"
        + "".join(f"[bench] shot {spot}\n" for spot in spots)
        + "[bench] done\n")
    textures = [{"kind": "texture", "name": material + ".TGA"} for material in materials]
    (directory / "census.json").write_text(json.dumps({
        "files": textures if original else [], "replacements": [] if original else textures,
    }))
    for spot in spots:
        (directory / f"{spot}.png").write_bytes(b"shot")
    return directory, scenario, index


@pytest.mark.parametrize("variant", ("pilot-3a", "pilot-3b"))
def test_iteration3_captures_require_window(tmp_path, variant):
    inputs = capture_check_inputs(tmp_path, variant, capture.MATERIALS)
    with pytest.raises(SystemExit, match="pilot textures not replaced: NW_CITY_WINDOW_03"):
        capture.check(*inputs)


@pytest.mark.parametrize("variant", ("original", "pilot-2", "pilot-3a", "pilot-3b"))
def test_iteration3_captures_validate_eight_targets(tmp_path, variant, capsys):
    original = variant == "original"
    inputs = capture_check_inputs(tmp_path, variant, capture.ITERATION3_MATERIALS, original=original)
    capture.check(*inputs, iteration3=variant in ("original", "pilot-2"))
    expected = "pilot_replaced=0 pilot_originals=8" if original else "pilot_replaced=8 pilot_originals=0"
    assert expected in capsys.readouterr().out


@pytest.mark.parametrize("variant", ("original", "pilot-2"))
def test_historical_captures_allow_seven_but_iteration3_requires_window(tmp_path, variant):
    inputs = capture_check_inputs(tmp_path, variant, capture.MATERIALS, original=variant == "original")
    capture.check(*inputs)
    with pytest.raises(SystemExit, match="NW_CITY_WINDOW_03"):
        capture.check(*inputs, iteration3=True)


def test_prepared_base_originals_are_accepted(tmp_path, capsys):
    inputs = capture_check_inputs(tmp_path, "original", capture.MATERIALS, original=True)
    census = inputs[0] / "census.json"
    data = json.loads(census.read_text())
    data["replacements"] = [dict(row, kind="basetexture") for row in data["files"]]
    data["files"] = []
    census.write_text(json.dumps(data))
    capture.check(*inputs)
    assert "pilot_replaced=0 pilot_originals=7" in capsys.readouterr().out


@pytest.mark.parametrize("source", ("archive", "base"))
def test_iteration3_rejects_opened_window_original(tmp_path, source):
    inputs = capture_check_inputs(tmp_path, "pilot-3b", capture.ITERATION3_MATERIALS)
    census = inputs[0] / "census.json"
    data = json.loads(census.read_text())
    row = {"kind": "texture" if source == "archive" else "basetexture", "name": "NW_CITY_WINDOW_03-C.TEX"}
    data["files" if source == "archive" else "replacements"].append(row)
    census.write_text(json.dumps(data))
    with pytest.raises(SystemExit, match="pilot originals opened: NW_CITY_WINDOW_03"):
        capture.check(*inputs)


@pytest.mark.parametrize("variant,count", (("original", 12), ("pilot-3b", 8), ("house-1", 12), ("house-2", 12)))
def test_house_captures_validate_three_views_and_target_census(tmp_path, variant, count, capsys):
    materials = capture.ITERATION3_MATERIALS if variant == "pilot-3b" else capture.HOUSE_MATERIALS
    inputs = capture_check_inputs(tmp_path, variant, materials, original=variant == "original", market_house=True)
    capture.check(*inputs, market_house=True)
    expected = f"pilot_replaced=0 pilot_originals={count}" if variant == "original" else f"pilot_replaced={count} pilot_originals=0"
    output = capsys.readouterr().out
    assert f"shots=3 materials={0 if variant == 'original' else 58}" in output
    assert expected in output


@pytest.mark.parametrize("variant", ("house-1", "house-2"))
def test_house_captures_reject_missing_basement_replacement(tmp_path, variant):
    inputs = capture_check_inputs(tmp_path, variant, capture.ITERATION3_MATERIALS, market_house=True)
    with pytest.raises(SystemExit, match="pilot textures not replaced: NW_CITY_FACHWERKBASEMENT"):
        capture.check(*inputs, market_house=True)


@pytest.mark.parametrize("variant", ("house-1", "house-2"))
def test_house_captures_reject_opened_basement_original(tmp_path, variant):
    inputs = capture_check_inputs(tmp_path, variant, capture.HOUSE_MATERIALS, market_house=True)
    census = inputs[0] / "census.json"
    data = json.loads(census.read_text())
    data["files"] = [{"kind": "texture", "name": "NW_CITY_FACHWERKBASEMENT_CHAIN_01-C.TEX"}]
    census.write_text(json.dumps(data))
    with pytest.raises(SystemExit, match="pilot originals opened: NW_CITY_FACHWERKBASEMENT_CHAIN_01"):
        capture.check(*inputs, market_house=True)


def test_harbour_game_rebuilds_stale_links_without_removing_real_files(tmp_path):
    source = tmp_path / "game-settings"
    old = tmp_path / "old-installation"
    target = tmp_path / "task/game"
    for directory in (source / "system", old / "system", target / "system"):
        directory.mkdir(parents=True)
    (source / "system/Gothic.ini").write_text("new settings")
    (source / "system/SystemPack.ini").write_text("[PARAMETERS]\nVerticalFOV=90\n")
    for name in ("Gothic.ini", "SystemPack.ini"):
        (old / "system" / name).write_text("old settings")
        (target / "system" / name).symlink_to(old / "system" / name)
    (target / "Data").symlink_to("/nonexistent/Data")
    for name in ("_work", "Saves", "vdfs.cfg"):
        (old / name).touch()
        (target / name).symlink_to(old / name)
    kept = (target / "keep.txt", target / "system/personal.ini")
    for path in kept:
        path.write_text("personal file")
    script = Path(__file__).with_name("capture-quality-pilot.sh").read_text()
    block = script.split("<<'PYGAME'\n", 1)[1].split("\nPYGAME", 1)[0]
    for _ in range(2):
        subprocess.run([sys.executable, "-c", block, str(source), str(target)], check=True)
        assert all(path.read_text() == "personal file" for path in kept)
        assert (target / "system/Gothic.ini").resolve() == source / "system/Gothic.ini"
        assert "VerticalFOV=125" in (target / "system/SystemPack.ini").read_text()
        assert not (target / "system/SystemPack.ini").is_symlink()
        assert (old / "system/SystemPack.ini").read_text() == "old settings"
        assert all(not (target / name).exists() and not (target / name).is_symlink()
                   for name in ("Data", "_work", "Saves", "vdfs.cfg"))
        links = [path for path in target.rglob("*") if path.is_symlink()]
        assert links and all(path.exists() and path.resolve().is_relative_to(source) for path in links)


def harbour_check_inputs(tmp_path):
    inputs = capture_check_inputs(tmp_path, "original", capture.HARBOUR_MATERIALS, original=True)
    spots = tuple(f"B{number:03}-{view}" for number in range(1, 7)
                  for view in ("front", "three-quarter"))
    inputs[1].write_text("".join(f"shot {spot}\n" for spot in spots))
    (inputs[0] / "game.log").write_text("materials: 0 entries from test\n"
        + "".join(f"[bench] shot {spot}\n" for spot in spots) + "[bench] done\n")
    for spot in spots:
        Image.new("RGB", (1280, 720)).save(inputs[0] / f"{spot}.png")
    return inputs


def test_harbour_captures_validate_twelve_original_frames(tmp_path, capsys):
    capture.check(*harbour_check_inputs(tmp_path), harbour_house=True)
    assert "shots=12 materials=0 pilot_replaced=0 pilot_originals=3" in capsys.readouterr().out


def test_harbour_captures_reject_incomplete_scenario(tmp_path):
    inputs = harbour_check_inputs(tmp_path)
    inputs[1].write_text("shot B001-front\n")
    with pytest.raises(SystemExit, match="expected twelve distinct harbour shots"):
        capture.check(*inputs, harbour_house=True)


def test_harbour_captures_reject_twelve_unpaired_views(tmp_path):
    inputs = harbour_check_inputs(tmp_path)
    inputs[1].write_text("".join(f"shot B{number:03}-front\n" for number in range(1, 13)))
    with pytest.raises(SystemExit, match="expected front and three-quarter for six harbour blocks"):
        capture.check(*inputs, harbour_house=True)


def test_harbour_captures_reject_unknown_view(tmp_path):
    inputs = harbour_check_inputs(tmp_path)
    inputs[1].write_text(inputs[1].read_text().replace("B001-front", "B001-side"))
    with pytest.raises(SystemExit, match="expected harbour shot names BLOCK-front or BLOCK-three-quarter"):
        capture.check(*inputs, harbour_house=True)


def test_harbour_captures_reject_wrong_frame_size(tmp_path):
    inputs = harbour_check_inputs(tmp_path)
    Image.new("RGB", (640, 360)).save(inputs[0] / "B003-three-quarter.png")
    with pytest.raises(SystemExit, match="shot B003-three-quarter: expected 1280x720"):
        capture.check(*inputs, harbour_house=True)


def test_harbour_captures_reject_nonzero_exit(tmp_path):
    inputs = harbour_check_inputs(tmp_path)
    (inputs[0] / "exit.txt").write_text("1\n")
    with pytest.raises(SystemExit, match="nonzero game exit"):
        capture.check(*inputs, harbour_house=True)


def test_harbour_captures_require_original_variant(tmp_path):
    inputs = harbour_check_inputs(tmp_path)
    variant = inputs[0].with_name("staging")
    inputs[0].rename(variant)
    with pytest.raises(SystemExit, match="harbour survey requires original materials"):
        capture.check(variant, *inputs[1:], harbour_house=True)


def test_harbour_captures_require_port_materials(tmp_path):
    inputs = harbour_check_inputs(tmp_path)
    census = inputs[0] / "census.json"
    data = json.loads(census.read_text())
    data["files"] = data["files"][:-1]
    census.write_text(json.dumps(data))
    with pytest.raises(SystemExit, match="pilot originals not opened: NW_CITY_HAFEN_GIEBEL_01"):
        capture.check(*inputs, harbour_house=True)


def hut_check_inputs(tmp_path):
    inputs = capture_check_inputs(tmp_path, "original", capture.HARBOUR_MATERIALS, original=True)
    inputs[1].write_text("".join(f"shot {spot}\n" for spot in capture.HUT_SPOTS))
    (inputs[0] / "game.log").write_text("materials: 0 entries from test\n"
        + "".join(f"[bench] shot {spot}\n" for spot in capture.HUT_SPOTS) + "[bench] done\n")
    for spot in capture.HUT_SPOTS:
        Image.new("RGB", (1280, 720)).save(inputs[0] / f"{spot}.png")
    return inputs


def test_hut_captures_validate_six_original_frames(tmp_path, capsys):
    capture.check(*hut_check_inputs(tmp_path), harbour_hut=True)
    assert "shots=6 materials=0 pilot_replaced=0 pilot_originals=3" in capsys.readouterr().out


@pytest.mark.parametrize("replacement", ("", "shot side\n", "shot front\n"))
def test_hut_captures_require_all_six_named_views(tmp_path, replacement):
    inputs = hut_check_inputs(tmp_path)
    inputs[1].write_text(inputs[1].read_text().replace("shot gable-close\n", replacement))
    with pytest.raises(SystemExit, match="unexpected shot list"):
        capture.check(*inputs, harbour_hut=True)


def test_hut_captures_reject_wrong_frame_size(tmp_path):
    inputs = hut_check_inputs(tmp_path)
    Image.new("RGB", (640, 360)).save(inputs[0] / "window.png")
    with pytest.raises(SystemExit, match="shot window: expected 1280x720"):
        capture.check(*inputs, harbour_hut=True)


def test_hut_staging_capture_requires_full_material_set(tmp_path):
    inputs = hut_check_inputs(tmp_path)
    variant = inputs[0].with_name("hut-1")
    inputs[0].rename(variant)
    with pytest.raises(SystemExit, match="expected 0 original or 58 staging materials"):
        capture.check(variant, *inputs[1:], harbour_hut=True)


def test_hut_captures_reject_material_replacement(tmp_path):
    inputs = hut_check_inputs(tmp_path)
    census = inputs[0] / "census.json"
    data = json.loads(census.read_text())
    data["replacements"] = [{"kind": "texture", "name": "NW_CITY_HAFEN_HOLZWAND_01.TGA"}]
    census.write_text(json.dumps(data))
    with pytest.raises(SystemExit, match="baseline replaces pilot materials"):
        capture.check(*inputs, harbour_hut=True)


def test_flat_frame_has_no_detail_and_uses_linear_rec709(tmp_path):
    path = tmp_path / "green.png"
    Image.new("RGB", (1280, 720), (0, 128, 0)).save(path)
    measured = capture.measure_frame(path)
    assert measured["mean_y"] == pytest.approx(.7152 * ((128 / 255 + .055) / 1.055) ** 2.4)
    assert measured["log_spread"] == 0
    assert measured["detail_k9"] == pytest.approx(0, abs=1e-12)


def test_detail_uses_whole_frame_and_reflected_box(tmp_path):
    path = tmp_path / "stripes.png"
    row = np.full(1280, 128, dtype=np.uint8)
    row[:7] = 16
    row[7:263] = 224
    image = np.broadcast_to(row[None, :, None], (720, 1280, 3))
    Image.fromarray(image).save(path)
    srgb = row.astype(np.float64) / 255
    y = np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)
    log_y = np.log(y + .001)
    box = np.convolve(np.pad(log_y, 4, mode="symmetric"), np.full(9, 1 / 9), mode="valid")
    measured = capture.measure_frame(path)
    assert measured["mean_y"] == pytest.approx(y.mean())
    assert measured["log_spread"] == pytest.approx(np.percentile(log_y, 90) - np.percentile(log_y, 10))
    assert measured["detail_k9"] == pytest.approx(np.abs(log_y - box).mean(), abs=1e-12)


def test_frame_size_is_not_rescaled(tmp_path):
    path = tmp_path / "small.png"
    Image.new("RGB", (640, 360)).save(path)
    with pytest.raises(ValueError, match="expected 1280x720, got 640x360"):
        capture.measure_frame(path)


def test_detail_csv_compares_each_spot_with_its_original(tmp_path, monkeypatch):
    def measure(path):
        baseline = 2 if path.stem == "facade" else 4
        scale = {"original": 1, "pilot-2": .75, "pilot-3a": .95, "pilot-3b": 1.05}[path.parent.name]
        return {"mean_y": baseline * scale, "log_spread": baseline * 2 * scale,
                "detail_k9": baseline * 3 * scale}

    monkeypatch.setattr(capture, "SPOTS", ("facade", "stairs"))
    monkeypatch.setattr(capture, "measure_frame", measure)
    output = tmp_path / "report" / "detail.csv"
    variants = ["original", "pilot-2", "pilot-3a", "pilot-3b"]
    capture.detail(tmp_path / "captures", output, variants)
    with output.open() as stream:
        rows = list(csv.DictReader(stream))
    assert [(row["spot"], row["variant"]) for row in rows] == [
        (spot, variant) for spot in ("facade", "stairs") for variant in variants]
    for row in rows:
        for field in ("mean_y", "log_spread", "detail_k9"):
            assert float(row[field + "_ratio"]) == pytest.approx(
                {"original": 1, "pilot-2": .75, "pilot-3a": .95, "pilot-3b": 1.05}[row["variant"]])


def test_iteration3_boards_keep_panel_width_and_variant_order(tmp_path):
    variants = ("original", "pilot-2", "pilot-3a", "pilot-3b")
    colours = ((220, 20, 20), (20, 220, 20), (20, 20, 220), (220, 220, 20))
    captures = tmp_path / "captures"
    for variant, colour in zip(variants, colours):
        (captures / variant).mkdir(parents=True)
        for spot in capture.SPOTS:
            Image.new("RGB", (1280, 720), colour).save(captures / variant / f"{spot}.png")
    output = tmp_path / "boards"
    capture.compose(captures, output, iteration3=True)
    assert {path.name for path in output.iterdir()} == {f"{spot}.jpg" for spot in capture.SPOTS}
    for path in output.iterdir():
        with Image.open(path) as board:
            assert board.format == "JPEG"
            assert board.size == (4 * 768, 432 + 44)
            for panel, colour in enumerate(colours):
                assert board.getpixel((panel * 768 + 384, 260)) == pytest.approx(colour, abs=3)


@pytest.mark.parametrize("house2", (False, True))
def test_house_boards_keep_three_full_frame_panels_in_order(tmp_path, house2):
    variants = ("original", "house-1", "house-2") if house2 else ("original", "pilot-3b", "house-1")
    colours = ((220, 20, 20), (20, 220, 20), (20, 20, 220))
    captures = tmp_path / "captures"
    for variant, colour in zip(variants, colours):
        (captures / variant).mkdir(parents=True)
        for spot in capture.HOUSE_SPOTS:
            Image.new("RGB", (1280, 720), colour).save(captures / variant / f"{spot}.png")
    output = tmp_path / "boards"
    if house2:
        output.mkdir()
        for spot in capture.HOUSE_SPOTS:
            (output / f"{spot}.jpg").write_bytes(b"existing JPEG")
    capture.compose(captures, output, market_house=not house2, market_house2=house2)
    suffix = "-house-2" if house2 else ""
    expected = {f"{spot}{suffix}.jpg" for spot in capture.HOUSE_SPOTS}
    assert {path.name for path in output.iterdir()} == (expected | {f"{spot}.jpg" for spot in capture.HOUSE_SPOTS})
    if house2:
        assert all((output / f"{spot}.jpg").read_bytes() == b"existing JPEG" for spot in capture.HOUSE_SPOTS)
    for name in expected:
        path = output / name
        with Image.open(path) as board:
            assert board.format == "JPEG"
            assert board.size == (3 * 768, 432 + 44)
            for panel, colour in enumerate(colours):
                assert board.getpixel((panel * 768 + 384, 260)) == pytest.approx(colour, abs=3)


def remaster_check_inputs(tmp_path, variant, textures):
    directory = tmp_path / variant
    directory.mkdir()
    (directory / "exit.txt").write_text("0\n")
    spots = ("roofs-harbour", "roofs-upper")
    scenario = tmp_path / "city-roofs.bench"
    scenario.write_text("".join(f"goto spot\nshot {spot}\n" for spot in spots))
    index = tmp_path / "materials.json"
    index.write_text(json.dumps({texture: {} for texture in textures}))
    (directory / "game.log").write_text(f"materials: {len(textures)} entries from test\n[bench] goto spot\n"
        + "".join(f"[bench] shot {spot}\n" for spot in spots) + "[bench] done\n")
    entries = [{"kind": "texture", "name": texture} for texture in textures]
    (directory / "census.json").write_text(json.dumps({"files": [], "replacements": entries}))
    for spot in spots:
        Image.new("RGB", (1280, 720)).save(directory / f"{spot}.png")
    return directory, scenario, index


def test_remaster_set_checked_against_its_own_index(tmp_path, capsys):
    inputs = remaster_check_inputs(tmp_path, "roofs-1", ["NW_CITY_ROOF_01.TGA", "NW_CITY_ROOF_04.TGA"])
    capture.check(*inputs, remaster=True)
    assert "roofs-1: shots=2 materials=2 pilot_replaced=2 pilot_originals=0" in capsys.readouterr().out
    census = inputs[0] / "census.json"
    census.write_text(json.dumps({"files": [{"kind": "texture", "name": "NW_CITY_ROOF_04.TGA"}],
                                  "replacements": [{"kind": "texture", "name": "NW_CITY_ROOF_01.TGA"}]}))
    with pytest.raises(SystemExit, match="pilot textures not replaced: NW_CITY_ROOF_04"):
        capture.check(*inputs, remaster=True)


def test_remaster_original_needs_empty_index(tmp_path, capsys):
    for folder in ("empty", "filled"):
        (tmp_path / folder).mkdir()
    capture.check(*remaster_check_inputs(tmp_path / "empty", "original", []), remaster=True)
    assert "original: shots=2 materials=0" in capsys.readouterr().out
    with pytest.raises(SystemExit, match="original needs an empty index"):
        capture.check(*remaster_check_inputs(tmp_path / "filled", "original", ["NW_CITY_ROOF_01.TGA"]), remaster=True)
