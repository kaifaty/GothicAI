"""Architectural sheet views retain the physical shape and bounded edges."""
from pathlib import Path
import importlib.util

import numpy as np
from PIL import Image


spec = importlib.util.spec_from_file_location("material_preview", Path(__file__).with_name("material_preview.py"))
preview = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preview)


def test_tall_beam_preview_restores_physical_proportions():
    # Square storage contains a complete beam, not a square piece of timber.
    square = Image.new("RGB", (16, 16), (200, 100, 60))
    aspect = 1.15 / 10.14
    panel = np.asarray(preview.module_panel(square, 128, aspect))
    material = np.all(panel == (200, 100, 60), axis=2)
    ys, xs = np.nonzero(material)
    assert np.ptp(ys) + 1 == 128
    assert abs((np.ptp(xs) + 1) / 128 - aspect) < 1 / 128


def test_panel_edges_clamp_and_strip_only_wraps_along_its_length():
    image = np.array([[[0.], [.25]], [[.75], [1.]]], dtype=np.float32)
    u, v = np.array([0., 1.]), np.array([1., 0.])
    # A bounded panel retains the distinct top-left and bottom-right corners.
    assert np.allclose(preview.sample_periodic(image, u, v, "")[:, 0], [0., 1.])
    # A vertical strip joins top/bottom while retaining its separate side edges.
    assert np.allclose(preview.sample_periodic(image, u, v, "v")[:, 0], [.375, .625])


def test_oblique_plane_uses_the_beam_shape():
    maps = {"albedo": np.full((8, 8, 3), .7), "normal": np.tile([.5, .5, 1.], (8, 8, 1))}
    maps.update({name: np.full((8, 8, 1), value)
                 for name, value in (("roughness", .8), ("metallic", 0.), ("ao", 1.))})
    square = np.asarray(preview.plane_preview(maps, 128, repeats=(1., 1.), wrap_axes=""))
    beam = np.asarray(preview.plane_preview(maps, 128, aspect=1.15 / 10.14,
                                          repeats=(1., 1.), wrap_axes="v"))
    background = np.rint(np.array([.125, .135, .15]) * 255).astype(np.uint8)
    square_pixels = np.count_nonzero(np.any(square != background, axis=2))
    beam_pixels = np.count_nonzero(np.any(beam != background, axis=2))
    assert 0 < beam_pixels < square_pixels * .2


def preview_maps(alpha=None):
    albedo = np.full((8, 8, 3), .7)
    if alpha is not None:
        albedo = np.concatenate((albedo, np.full((8, 8, 1), alpha)), axis=2)
    maps = {"albedo": albedo, "normal": np.tile([.5, .5, 1.], (8, 8, 1))}
    maps.update({name: np.full((8, 8, 1), value)
                 for name, value in (("roughness", .8), ("metallic", 0.), ("ao", 1.))})
    return maps


def test_read_maps_preserves_source_albedo_alpha_only(tmp_path):
    files = {}
    for channel in preview.REQUIRED_MAPS:
        destination = tmp_path / f"{channel}.png"
        if channel == "albedo":
            rgba = np.full((8, 8, 4), (170, 120, 80, 128), dtype=np.uint8)
            rgba[:, :4, 3] = 0
            Image.fromarray(rgba).save(destination)
        elif channel == "normal":
            Image.new("RGB", (8, 8), (128, 128, 255)).save(destination)
        else:
            Image.new("L", (8, 8), 180).save(destination)
        files[channel] = destination.name
    maps, reason = preview.read_maps({"id": "ivy", "files": files}, tmp_path)
    assert reason is None
    assert maps["albedo"].shape == (8, 8, 4)
    assert np.array_equal(np.rint(maps["albedo"] * 255).astype(np.uint8), rgba)
    assert maps["normal"].shape == (8, 8, 3)
    assert maps["ao"].shape == (8, 8, 1)
    Image.new("RGB", (8, 8), (170, 120, 80)).save(tmp_path / files["albedo"])
    opaque, _ = preview.read_maps({"id": "rope", "files": files}, tmp_path)
    assert opaque["albedo"].shape == (8, 8, 3)


def test_albedo_cutout_uses_checkerboard_and_binary_half_cutoff():
    rgba = np.full((24, 24, 4), (220, 30, 120, 128), dtype=np.uint8)
    rgba[:, :12, 3] = 127
    shown = np.asarray(preview.alpha_preview(Image.fromarray(rgba)))
    assert np.array_equal(shown[0, 0], preview.CHECKER_COLOURS[0])
    assert np.array_equal(shown[12, 0], preview.CHECKER_COLOURS[1])
    assert np.all(shown[:, 12:] == (220, 30, 120))
    # Discarded saturated RGB never tints the backing.
    assert np.all(shown[:, :12] == np.asarray(preview.checkerboard((24, 24)))[:, :12])


def test_alpha_module_and_tiled_albedo_show_the_checkerboard():
    image = Image.new("RGBA", (16, 16), (255, 0, 255, 0))
    tiled = np.asarray(preview.tiled_panel(image, 48))
    module = np.asarray(preview.module_panel(image, 48, 1.))
    checker = np.asarray(preview.checkerboard((48, 48)))
    assert np.array_equal(tiled, checker)
    assert np.array_equal(module, checker)


def test_lit_alpha_cutoff_shows_unlit_checkerboard_and_keeps_opaque_render():
    opaque = np.asarray(preview.plane_preview(preview_maps(), 128, repeats=(1., 1.), wrap_axes=""))
    at_cutoff = np.asarray(preview.plane_preview(preview_maps(.5), 128, repeats=(1., 1.), wrap_axes=""))
    discarded = np.asarray(preview.plane_preview(preview_maps(.49), 128, repeats=(1., 1.), wrap_axes=""))
    assert np.array_equal(at_cutoff, opaque)
    background = np.rint(np.array([.125, .135, .15]) * 255).astype(np.uint8)
    on_plane = np.any(opaque != background, axis=2)
    checker = np.asarray(preview.checkerboard((128, 128)))
    assert np.array_equal(discarded[on_plane], checker[on_plane])
    assert np.array_equal(discarded[~on_plane], opaque[~on_plane])
    assert len(np.unique(discarded[on_plane], axis=0)) == 2


def test_alpha_sheet_labels_game_cutoff_and_displays_holes(tmp_path, monkeypatch):
    maps = preview_maps(0.)
    monkeypatch.setattr(preview, "read_maps", lambda record, root: (maps, None))
    texts = []
    original = preview.ImageDraw.ImageDraw.text
    def record_text(draw, xy, text, *args, **kwargs):
        texts.append(text)
        return original(draw, xy, text, *args, **kwargs)
    monkeypatch.setattr(preview.ImageDraw.ImageDraw, "text", record_text)
    record = {"id": "ivy", "pipeline": "imagegen-filter", "replaces": ["IVY.TGA"],
              "parameters": {"decision": "panel", "material_class": "foliage_panel",
                             "source_aspect": 1., "tiling_axes": ""}}
    destination = preview.make_set_sheet("city-3", [record], tmp_path, tmp_path, 128)
    assert "alpha cutoff 0.5; albedo only in game" in texts
    with Image.open(destination) as sheet:
        pixels = np.asarray(sheet).astype(np.int16)
    # The first albedo panel begins at (18, 152), beneath its caption.
    assert np.max(np.abs(pixels[154, 20] - preview.CHECKER_COLOURS[0])) <= 3
    assert np.max(np.abs(pixels[154, 32] - preview.CHECKER_COLOURS[1])) <= 3
