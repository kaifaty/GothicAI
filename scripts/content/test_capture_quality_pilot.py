"""Full-frame pilot statistics and their per-spot baseline ratios."""

import csv
import importlib.util
import json
from pathlib import Path

import numpy as np
from PIL import Image
import pytest


SPEC = importlib.util.spec_from_file_location(
    "capture_quality_pilot", Path(__file__).with_name("capture-quality-pilot.py"))
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


def capture_check_inputs(tmp_path, variant, materials, *, original=False):
    directory = tmp_path / variant
    directory.mkdir()
    (directory / "exit.txt").write_text("0\n")
    scenario = tmp_path / "pilot.bench"
    scenario.write_text("".join(f"shot {spot}\n" for spot in capture.SPOTS))
    index = tmp_path / "materials.json"
    index.write_text(json.dumps({} if original else {"test": {}}))
    (directory / "game.log").write_text(
        f"materials: {0 if original else 1} entries from test\n"
        + "".join(f"[bench] shot {spot}\n" for spot in capture.SPOTS)
        + "[bench] done\n")
    textures = [{"kind": "texture", "name": material + ".TGA"} for material in materials]
    (directory / "census.json").write_text(json.dumps({
        "files": textures if original else [], "replacements": [] if original else textures,
    }))
    for spot in capture.SPOTS:
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


def test_iteration3_rejects_opened_window_original(tmp_path):
    inputs = capture_check_inputs(tmp_path, "pilot-3b", capture.ITERATION3_MATERIALS)
    census = inputs[0] / "census.json"
    data = json.loads(census.read_text())
    data["files"] = [{"kind": "texture", "name": "NW_CITY_WINDOW_03-C.TEX"}]
    census.write_text(json.dumps(data))
    with pytest.raises(SystemExit, match="pilot originals opened: NW_CITY_WINDOW_03"):
        capture.check(*inputs)


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
