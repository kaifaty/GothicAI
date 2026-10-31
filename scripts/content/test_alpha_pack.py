"""DDS format selection and decoded cutout coverage, including tiny mipmaps."""
import importlib.util
import json
from pathlib import Path
import struct

import numpy as np
from PIL import Image
import pytest

import pack_materials as packer


spec = importlib.util.spec_from_file_location("alpha_fixture", Path(__file__).with_name("test-material.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def decoded_bc3_mips(path):
    """Decode the actual stored BC3 palettes/selectors, not prefilter arrays."""
    data = path.read_bytes()
    assert data[:4] == b"DDS " and data[84:88] == b"DXT5"
    h, w = struct.unpack_from("<2I", data, 12)
    levels = struct.unpack_from("<I", data, 28)[0]
    offset = 128
    result = []
    for _ in range(levels):
        image = np.zeros((((h+3)//4)*4, ((w+3)//4)*4, 4), dtype=np.uint8)
        for y in range((h+3)//4):
            for x in range((w+3)//4):
                block = data[offset:offset+16]
                offset += 16
                a0, a1 = block[:2]
                if a0 > a1:
                    alpha = [a0, a1] + [((7-i)*a0+i*a1)//7 for i in range(1, 7)]
                else:
                    alpha = [a0, a1] + [((5-i)*a0+i*a1)//5 for i in range(1, 5)] + [0, 255]
                indices = int.from_bytes(block[2:8], "little")
                colours = fixture.from565(np.array(struct.unpack_from("<2H", block, 8), dtype=np.uint16))
                p0, p1 = colours
                palette = [p0, p1, (2*p0+p1)//3, (p0+2*p1)//3]
                selectors = int.from_bytes(block[12:16], "little")
                for pixel in range(16):
                    yy, xx = y*4+pixel//4, x*4+pixel%4
                    image[yy, xx, :3] = palette[(selectors >> (2*pixel)) & 3]
                    image[yy, xx, 3] = alpha[(indices >> (3*pixel)) & 7]
        result.append(image[:h, :w])
        h, w = max(1, h//2), max(1, w//2)
    assert offset == len(data)
    return result


@pytest.mark.parametrize("alpha", [1., 254/255, .5])
def test_rgba_without_cutouts_stays_dxt1_and_preserves_opaque_bytes(tmp_path, alpha):
    rgb = np.random.default_rng(13).random((16, 16, 3))
    rgba = np.dstack((rgb, np.full((16, 16), alpha)))
    fixture.write_dds(tmp_path / "rgb.dds", rgb)
    fixture.write_dds(tmp_path / "rgba.dds", rgba)
    assert (tmp_path / "rgba.dds").read_bytes()[84:88] == b"DXT1"
    assert (tmp_path / "rgb.dds").read_bytes() == (tmp_path / "rgba.dds").read_bytes()


@pytest.mark.parametrize("seed,coverage", [(1, .37), (2, .51), (4, .83), (7, .12)])
def test_decoded_dxt5_all_mips_keep_hard_alpha_coverage(tmp_path, seed, coverage):
    rgba = np.full((32, 32, 4), (.25, .6, .15, 0.))
    rgba[..., 3] = np.random.default_rng(seed).random((32, 32)) < coverage
    fixture.write_dds(tmp_path / "cutout.dds", rgba)
    mips = decoded_bc3_mips(tmp_path / "cutout.dds")
    assert [mip.shape[0] for mip in mips] == [32, 16, 8, 4, 2, 1]
    base = (mips[0][..., 3] >= 128).mean()
    for mip in mips:
        assert set(np.unique(mip[..., 3])) <= {0, 255}
        assert (mip[..., 3] >= 128).mean() + 1e-12 >= base - .10


def test_cutoff_boundary_and_single_texel_foreground_have_no_rgb_halo(tmp_path):
    rgba = np.ones((16, 16, 4))
    rgba[::2, :, :3] = 0  # black/white hidden RGB must never enter mips.
    rgba[..., 3] = 127/255
    rgba[8, 8] = (.25, .6, .15, 128/255)
    fixture.write_dds(tmp_path / "one.dds", rgba)
    mips = decoded_bc3_mips(tmp_path / "one.dds")
    assert np.count_nonzero(mips[0][..., 3] >= 128) == 1
    for mip in mips:
        assert np.max(np.abs(mip[..., :3]/255 - (.25, .6, .15))) < .04


@pytest.mark.parametrize("transparent", [False, True])
def test_packing_preserves_png_alpha_and_indexes_normal_orm(tmp_path, transparent):
    root = tmp_path / "source"
    root.mkdir()
    files = {}
    for channel in packer.MAPS:
        target = root / f"{channel}.png"
        if channel == "albedo":
            pixels = np.full((16, 16, 4), (80, 140, 40, 255), dtype=np.uint8)
            if transparent:
                pixels[:, :9, 3] = 0
            Image.fromarray(pixels).save(target)
        elif channel == "normal":
            Image.new("RGB", (16, 16), (128, 128, 255)).save(target)
        else:
            Image.new("L", (16, 16), 220 if channel != "metallic" else 0).save(target)
        files[channel] = target.name
    record = {"id": "ivy", "files": files, "replaces": ["IVY.TGA"],
              "parameters": {"scale": 1}}
    assert packer.read_maps(record, root)["albedo"].shape == (16, 16, 4)
    output = packer.pack_set([record], root, tmp_path / "staging")
    entry = json.loads((output / "materials/materials.json").read_text())["IVY.TGA"]
    assert set(entry) == {"albedo", "normal", "orm", "scale"}
    expected = {"albedo": b"DXT5" if transparent else b"DXT1",
                "normal": b"DXT5", "orm": b"DXT1"}
    for channel, fourcc in expected.items():
        assert (output / "materials" / entry[channel]).read_bytes()[84:88] == fourcc


def test_pack_reads_indexed_png_transparency(tmp_path):
    files = {}
    for channel in packer.MAPS:
        path = tmp_path / f"{channel}.png"
        image = Image.new("RGB" if channel in ("albedo", "normal") else "L",
                          (16, 16), (128, 128, 255) if channel in ("albedo", "normal") else 255)
        if channel == "albedo":
            image = image.convert("P")
            image.save(path, transparency=0)
        else:
            image.save(path)
        files[channel] = path.name
    assert packer.read_maps({"id": "indexed", "files": files}, tmp_path)["albedo"].shape == (16, 16, 4)
