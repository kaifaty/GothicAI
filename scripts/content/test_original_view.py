import struct

import pytest
from PIL import Image

import original_view as ov


def ztex(width: int, height: int, levels: list[bytes], fmt: int = 10) -> bytes:
    return struct.pack('<4s8I', b'ZTEX', 0, fmt, width, height, len(levels), width, height, 0) + b''.join(levels)


def vdf(path, entries: dict[str, bytes]) -> None:
    root = 256 + 16 + 24
    data_at = root + 80 * len(entries)
    table, blobs = b'', b''
    for i, (name, blob) in enumerate(entries.items()):
        kind = 0x40000000 if i == len(entries) - 1 else 0
        table += struct.pack('<64s4I', name.ljust(64).encode('latin1'), data_at + len(blobs), len(blob), kind, 0)
        blobs += blob
    path.write_bytes(bytes(256) + b'PSVDSC_V2.00\r\n\r\n'
                     + struct.pack('<6I', len(entries), len(entries), 0, len(blobs), root, 80) + table + blobs)


RED = struct.pack('<2HI', 0xF800, 0x001F, 0)          # a DXT1 block: every texel is colour 0, red
BLUE = struct.pack('<2HI', 0x001F, 0x0000, 0)


def test_takes_the_top_level_and_the_later_archive(tmp_path):
    (tmp_path / 'Data').mkdir()
    # 8x4: the 4x2 level (one block) is stored first, then the top level (two blocks)
    vdf(tmp_path / 'Data' / 'Textures.vdf', {'NW_TEST-C.TEX': ztex(8, 4, [BLUE, RED + RED]),
                                             'NW_OLD-C.TEX': ztex(4, 4, [BLUE])})
    vdf(tmp_path / 'Data' / 'Textures_Addon.vdf', {'NW_OLD-C.TEX': ztex(4, 4, [RED])})
    out = tmp_path / 'out'
    out.mkdir()
    paths = ov.native(['nw_test.tga', 'NW_OLD'], tmp_path, out)
    assert [p.name for p in paths] == ['NW_OLD-native.png', 'NW_TEST-native.png']
    for path in paths:
        image = Image.open(path)
        assert image.mode == 'RGB'
        r, g, b = image.getpixel((0, 0))
        assert r > 240 and g < 10 and b < 10
    assert Image.open(paths[1]).size == (8, 4)


def test_missing_texture_and_unknown_format(tmp_path):
    (tmp_path / 'Data').mkdir()
    vdf(tmp_path / 'Data' / 'Textures.vdf', {'NW_TEST-C.TEX': ztex(4, 4, [RED])})
    with pytest.raises(SystemExit, match='NW_NONE-C.TEX'):
        ov.native(['NW_NONE'], tmp_path, tmp_path)
    with pytest.raises(ValueError, match='format 9'):
        ov.decode(ztex(4, 4, [bytes(16)], fmt=9))


def test_refuses_an_output_inside_the_repository():
    with pytest.raises(SystemExit, match='out of the repository'):
        ov.main(['NW_TEST', '--out', str(ov.ROOT / 'reports')])
