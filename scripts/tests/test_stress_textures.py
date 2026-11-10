"""stress_textures: the enlarged top level decodes to the old one with every texel repeated 2x2."""
import json
import random
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import stress_textures as st  # noqa: E402


def dds(fourcc: bytes, width: int, height: int, levels: list[bytes]) -> bytes:
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<IIIIIII", header, 4, 124, 0x1007, height, width, len(levels[0]), 0, len(levels))
    struct.pack_into("<II", header, 76, 32, 4)
    header[84:88] = fourcc
    struct.pack_into("<I", header, 108, 0x1000)
    return bytes(header) + b"".join(levels)


def chain(rng: random.Random, fourcc: bytes, width: int, height: int) -> list[bytes]:
    size = st.FORMATS[fourcc][0]
    levels = []
    while True:
        blocks = max(1, (width + 3) // 4) * max(1, (height + 3) // 4)
        levels.append(bytes(rng.randrange(256) for _ in range(blocks * size)))
        if width == 1 and height == 1:
            return levels
        width, height = max(1, width // 2), max(1, height // 2)


def rgb565(value: int) -> tuple[int, int, int]:
    return (value >> 11) & 31, (value >> 5) & 63, value & 31


def colour_block(block: bytes, dxt1: bool) -> list[tuple]:
    c0, c1, bits = struct.unpack("<HHI", block)
    a, b = rgb565(c0), rgb565(c1)
    if c0 > c1 or not dxt1:   # values stay symbolic: the test compares selections, not rounding
        palette = [(a, 255), (b, 255), ("2a+b", a, b, 255), ("a+2b", a, b, 255)]
    else:
        palette = [(a, 255), (b, 255), ("a+b", a, b, 255), ("transparent", 0)]
    return [palette[(bits >> (2 * i)) & 3] for i in range(16)]


def alpha_block(block: bytes, fourcc: bytes) -> list:
    if fourcc == b"DXT3":
        value = int.from_bytes(block[:8], "little")
        return [(value >> (4 * i)) & 15 for i in range(16)]
    value = int.from_bytes(block[2:8], "little")
    return [(block[0], block[1], (value >> (3 * i)) & 7) for i in range(16)]


def decode(level: bytes, width: int, height: int, fourcc: bytes) -> list[list]:
    size = st.FORMATS[fourcc][0]
    image = [[None] * width for _ in range(height)]
    for by in range(height // 4):
        for bx in range(width // 4):
            block = level[(by * (width // 4) + bx) * size:][:size]
            colour = colour_block(block[-8:], fourcc == b"DXT1")
            alpha = alpha_block(block, fourcc) if fourcc != b"DXT1" else [None] * 16
            for i in range(16):
                image[by * 4 + i // 4][bx * 4 + i % 4] = (colour[i], alpha[i])
    return image


@pytest.mark.parametrize("fourcc", [b"DXT1", b"DXT3", b"DXT5"])
@pytest.mark.parametrize("width,height", [(8, 8), (16, 4), (4, 12)])
def test_top_level_is_the_old_one_repeated(fourcc, width, height):
    levels = chain(random.Random(7), fourcc, width, height)
    source = dds(fourcc, width, height, levels)
    out = st.enlarge(source, 1)

    out_height, out_width, linear = struct.unpack_from("<III", out, 12)
    assert (out_width, out_height) == (width * 2, height * 2)
    assert struct.unpack_from("<I", out, 28)[0] == len(levels) + 1
    assert linear == len(levels[0]) * 4
    assert out[128 + linear:] == source[128:]          # the old chain follows untouched

    old = decode(levels[0], width, height, fourcc)
    new = decode(out[128:128 + linear], out_width, out_height, fourcc)
    for y in range(out_height):
        for x in range(out_width):
            assert new[y][x] == old[y // 2][x // 2]


def test_two_steps_equal_one_step_twice():
    source = dds(b"DXT1", 8, 8, chain(random.Random(3), b"DXT1", 8, 8))
    assert st.enlarge(source, 2) == st.enlarge(st.enlarge(source, 1), 1)


@pytest.mark.parametrize("data,reason", [
    (b"PNG " + bytes(200), "not a DDS"),
    (dds(b"DXT1", 8, 8, [bytes(32)]), "no mip chain"),
    (dds(b"ATI2", 8, 8, [bytes(32), bytes(8)]), "format b'ATI2'"),
    (dds(b"DXT1", 6, 8, [bytes(32), bytes(8)]), "side is not a multiple of 4"),
])
def test_left_out(data, reason):
    with pytest.raises(st.Skip, match=reason.replace("'", ".")):
        st.enlarge(data, 1)


def test_run_writes_and_counts(tmp_path):
    source = tmp_path / "textures"
    source.mkdir()
    levels = chain(random.Random(1), b"DXT3", 8, 8)
    (source / "WOOD.dds").write_bytes(dds(b"DXT3", 8, 8, levels))
    (source / "._WOOD.dds").write_bytes(b"junk")
    (source / "FONT.dds").write_bytes(dds(b"DXT1", 8, 8, [bytes(32)]))
    (source / "textures.json").write_text("{}")
    out = tmp_path / "out"

    report = st.run(source, out, 4)

    assert sorted(p.name for p in out.iterdir()) == ["WOOD.dds", "stress.json"]
    assert report["written"] == 1 and report["skipped"] == {"no mip chain": 1}
    assert struct.unpack_from("<II", (out / "WOOD.dds").read_bytes(), 12) == (32, 32)
    assert json.loads((out / "stress.json").read_text())["bytes_after"] == report["bytes_after"]
    with pytest.raises(SystemExit, match="not empty"):
        st.run(source, out, 2)
    with pytest.raises(SystemExit, match="outside"):
        st.run(source, source / "inner", 2)
