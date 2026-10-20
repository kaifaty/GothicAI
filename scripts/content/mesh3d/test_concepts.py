"""Offline checks for exact source retention and the 5 × 2 pilot layout."""

import hashlib
import importlib.util
import json
from pathlib import Path

from PIL import Image, PngImagePlugin
import pytest


SPEC = importlib.util.spec_from_file_location("mesh3d_concepts", Path(__file__).with_name("concepts.py"))
concepts = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(concepts)

UTC = "2026-10-20T12:00:00Z"
TARGET = dict(id="barrel", visual="NW_HARBOUR_BARREL_01.MRM")


def png(path, color=(120, 80, 35, 255)):
    image = Image.new("RGBA", (100, 80))
    image.paste(color, (20, 10, 80, 70))
    metadata = PngImagePlugin.PngInfo()
    metadata.add_text("tool-metadata", "Keep these exact generator bytes.")
    image.save(path, pnginfo=metadata, compress_level=2)
    return path


def imported(tmp_path, **kwargs):
    source = png(tmp_path / "tool.png")
    registry = tmp_path / "registry.jsonl"
    source_root = tmp_path / "sources"
    record = concepts.import_concept(TARGET, source, 1, "A squat barrel.", UTC,
        registry_path=registry, source_root=source_root, **kwargs)
    return record, registry, source_root, source


def test_transparent_png_records_real_alpha_and_visible_bounds(tmp_path):
    metrics = concepts.validate_png(png(tmp_path / "tool.png"))
    assert metrics == dict(width=100, height=80, transparent_fraction=0.55, alpha_cutoff=128,
                           visible_bbox=[20, 10, 80, 70])


def test_opaque_checkerboard_is_not_transparency(tmp_path):
    source = tmp_path / "opaque.png"
    Image.new("RGB", (100, 80), (220, 220, 220)).save(source)
    with pytest.raises(ValueError, match="no alpha"):
        concepts.validate_png(source)


def test_alpha_requires_strictly_more_than_thirty_percent_fully_clear(tmp_path):
    source = tmp_path / "threshold.png"
    image = Image.new("RGBA", (20, 20))
    for x, y in [(x, y) for y in range(1, 19) for x in range(1, 19)][:280]:
        image.putpixel((x, y), (120, 80, 35, 255))
    image.save(source)
    with pytest.raises(ValueError, match="more than 30%"):
        concepts.validate_png(source)
    # Almost-transparent pixels do not count as fully transparent background.
    image = Image.new("RGBA", (20, 20), (0, 0, 0, 1))
    image.paste((120, 80, 35, 255), (5, 5, 15, 15))
    image.save(source)
    with pytest.raises(ValueError, match="got 0.0%"):
        concepts.validate_png(source)


@pytest.mark.parametrize("point", [(0, 40), (99, 40), (50, 0), (50, 79)])
def test_any_visible_pixel_on_each_edge_rejects_crop(tmp_path, point):
    source = png(tmp_path / "cropped.png")
    with Image.open(source) as opened:
        image = opened.copy()
    image.putpixel(point, (120, 80, 35, 128))
    image.save(source)
    with pytest.raises(ValueError, match="touches the image edge"):
        concepts.validate_png(source)


def test_nearly_transparent_background_noise_is_not_the_object(tmp_path):
    source = png(tmp_path / "noise.png")
    with Image.open(source) as opened:
        image = opened.copy()
    image.putpixel((0, 40), (120, 80, 35, 1))
    image.save(source)
    metrics = concepts.validate_png(source)
    assert metrics["visible_bbox"] == [20, 10, 80, 70]
    assert metrics["transparent_fraction"] == pytest.approx(0.55 - 1 / 8000)


def test_empty_object_and_wrong_format_are_rejected(tmp_path):
    source = tmp_path / "empty.png"
    Image.new("RGBA", (10, 10)).save(source)
    with pytest.raises(ValueError, match="no visible object"):
        concepts.validate_png(source)
    Image.new("RGB", (10, 10)).save(source, format="JPEG")
    with pytest.raises(ValueError, match="must be a PNG"):
        concepts.validate_png(source)


def test_import_retains_exact_bytes_metadata_checksum_and_one_recipe(tmp_path):
    record, registry, source_root, source = imported(tmp_path)
    retained = source_root / "mesh3d-pilot/barrel-v1.png"
    assert retained.read_bytes() == source.read_bytes()
    assert record["source_sha256"] == hashlib.sha256(source.read_bytes()).hexdigest()
    assert record["kind"] == "concept"
    assert record["seed"] is None and record["files"] == {}
    assert record["utc"] == UTC
    assert record["replaces"] == [TARGET["visual"]]
    assert record["models"][0]["id"] == "image_gen.imagegen"
    assert concepts.registry_module.check_registry(registry, source_root=source_root) == [record]
    # A retry with the same recipe does not append a second registry row.
    assert concepts.import_concept(TARGET, source, 1, "A squat barrel.", UTC,
        registry_path=registry, source_root=source_root) == record
    assert len(registry.read_text().splitlines()) == 1


def test_immutable_slot_and_recipe_cannot_be_replaced(tmp_path):
    source_root = tmp_path / "sources"
    retained = source_root / "mesh3d-pilot/barrel-v1.png"
    retained.parent.mkdir(parents=True)
    png(retained, (1, 2, 3, 255))
    original = retained.read_bytes()
    source = png(tmp_path / "tool.png")
    registry = tmp_path / "registry.jsonl"
    with pytest.raises(ValueError, match="refusing to overwrite"):
        concepts.import_concept(TARGET, source, 1, "A barrel.", UTC, registry, source_root)
    assert retained.read_bytes() == original
    assert not registry.exists()
    retained.unlink()
    concepts.import_concept(TARGET, source, 1, "A barrel.", UTC, registry, source_root)
    previous = registry.read_bytes()
    with pytest.raises(ValueError, match="different recipe"):
        concepts.import_concept(TARGET, source, 1, "A new barrel.", UTC, registry, source_root)
    assert registry.read_bytes() == previous
    assert retained.read_bytes() == source.read_bytes()


def test_import_detects_changed_recorded_source(tmp_path):
    record, registry, source_root, _ = imported(tmp_path)
    (source_root / record["source_png"]).write_bytes(b"changed PNG")
    with pytest.raises(ValueError, match="source_sha256: mismatch"):
        concepts.registry_module.check_registry(registry, source_root=source_root)


def test_source_symlink_cannot_escape_permanent_store(tmp_path):
    source_root = tmp_path / "sources"
    source_root.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (source_root / "mesh3d-pilot").symlink_to(outside, target_is_directory=True)
    source = png(tmp_path / "tool.png")
    with pytest.raises(ValueError, match="escapes the permanent store"):
        concepts.import_concept(TARGET, source, 1, "A barrel.", UTC,
            tmp_path / "registry.jsonl", source_root)
    assert list(outside.iterdir()) == []


@pytest.mark.parametrize("variant", [0, 10, True])
def test_variant_must_be_a_small_integer(tmp_path, variant):
    with pytest.raises(ValueError, match="variant must be an integer 1..9"):
        concepts.import_concept(TARGET, tmp_path / "unused", variant, "A barrel.", UTC)


def test_views_and_edits_of_our_own_concept(tmp_path):
    source_root = tmp_path / "sources"
    registry = tmp_path / "registry.jsonl"
    first = concepts.import_concept(TARGET, png(tmp_path / "a.png"), 3, "A barrel.", UTC, registry, source_root,
                                    view="34")
    front = concepts.import_concept(TARGET, png(tmp_path / "b.png", (90, 60, 40, 255)), 3, "Front of it.", UTC,
                                    registry, source_root, view="front", edit_of=first["id"])
    assert first["id"].endswith("-v3-34") and front["id"].endswith("-v3-front")
    assert front["parameters"]["edit_of"] == first["id"]
    assert (source_root / front["source_png"]).exists()
    with pytest.raises(ValueError, match="edit_of"):
        concepts.import_concept(TARGET, png(tmp_path / "c.png"), 3, "Top.", UTC, registry, source_root,
                                view="34", edit_of=first["id"])
    wide = tmp_path / "wide.png"  # an orthographic face: 19% transparent, free border
    image = Image.new("RGBA", (100, 100))
    image.paste((120, 80, 35, 255), (5, 5, 95, 95))
    image.save(wide)
    top = concepts.import_concept(TARGET, wide, 3, "Top.", UTC, registry, source_root, view="top", edit_of=first["id"])
    assert top["generation"]["alpha"]["transparent_fraction"] < 0.30
    with pytest.raises(ValueError, match="more than 30%"):
        concepts.import_concept(TARGET, wide, 4, "Wide.", UTC, registry, source_root, view="34")
    with pytest.raises(ValueError, match="view must be"):
        concepts.import_concept(TARGET, png(tmp_path / "d.png"), 3, "Back.", UTC, registry, source_root, view="back")


def test_contact_sheet_has_five_columns_two_rows_and_preserves_sources(tmp_path):
    targets = [dict(id=name, visual=name.upper() + ".MRM")
        for name in ("barrel", "crate", "table", "chest", "bench")]
    registry = tmp_path / "registry.jsonl"
    source_root = tmp_path / "sources"
    before = {}
    colors = [(150, 30, 20, 255), (30, 150, 20, 255), (20, 30, 150, 255),
              (150, 150, 20, 255), (150, 30, 150, 255)]
    for target, color in zip(targets, colors):
        for variant in (1, 2):
            source = png(tmp_path / f"{target['id']}-{variant}.png", color)
            record = concepts.import_concept(target, source, variant, "An isolated object.", UTC,
                registry, source_root)
            retained = source_root / record["source_png"]
            before[retained] = retained.read_bytes()
    output = tmp_path / "report/concepts.jpg"
    concepts.make_contact_sheet(targets, registry, source_root, output)
    with Image.open(output) as sheet:
        assert sheet.format == "JPEG"
        assert sheet.size == (1600, 704)
        for column, color in enumerate(colors):
            for row in (0, 1):
                actual = sheet.getpixel((column * 320 + 160, row * 352 + 160))
                assert max(abs(a - b) for a, b in zip(actual, color[:3])) <= 4
    assert all(path.read_bytes() == data for path, data in before.items())
    missing = targets.copy()
    missing[0] = dict(id="missing", visual="MISSING.MRM")
    with pytest.raises(ValueError, match="missing contact sheet concept"):
        concepts.make_contact_sheet(missing, registry, source_root, output)


def test_cli_import_invokes_existing_backup_with_matching_paths(tmp_path, monkeypatch):
    targets = [dict(id=f"item-{i}", visual=f"ITEM_{i}.MRM") for i in range(5)]
    target_file = tmp_path / "targets.json"
    target_file.write_text(json.dumps(targets))
    source = png(tmp_path / "tool.png")
    prompt = tmp_path / "prompt.txt"
    prompt.write_text("An isolated wooden item.")
    registry, source_root = tmp_path / "registry.jsonl", tmp_path / "sources"
    calls = []
    monkeypatch.setattr(concepts.subprocess, "run", lambda args, **kwargs: calls.append((args, kwargs)))
    result = concepts.main(["import", "--target", "item-0", "--png", str(source),
        "--variant", "1", "--prompt-file", str(prompt), "--utc", UTC,
        "--targets", str(target_file), "--registry", str(registry), "--source-root", str(source_root)])
    assert result == 0
    assert len(calls) == 1
    assert Path(calls[0][0][0]).name == "backup-sources.sh"
    assert calls[0][0][1:] == ["--registry", str(registry), "--source-root", str(source_root)]
    assert calls[0][1] == {"check": True}
