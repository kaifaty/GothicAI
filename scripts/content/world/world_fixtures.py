"""Helpers of the world edit tests (test_world_edit.py, test_world_edit_region.py): synthetic chunks as the
converter writes them, the mark of the tests that need Blender, and a stand-in for content-convert's cook."""
import hashlib
import json
import struct
import sys

import numpy as np
import pytest

import world_edit

MATERIALS = [{'name': 'TEST_A', 'texture': 'TEST_A.TGA'}, {'name': 'TEST_B', 'texture': 'TEST_B.TGA'}]


def write_glb(path, primitives, materials=MATERIALS):
    """A chunk as the converter writes it: per primitive POSITION, NORMAL, TEXCOORD_0, _VERTEX, indices; a
    primitive with a fifth item, the vertex light (0..1 per vertex), also gets _FEATURE and COLOR_0 (RGBA bytes)."""
    binary = bytearray()
    views, accessors, prims = [], [], []

    def add(array, kind, component, target=None, minmax=False):
        array = np.ascontiguousarray(array)
        while len(binary) % 4:
            binary.append(0)
        view = {'buffer': 0, 'byteOffset': len(binary), 'byteLength': array.nbytes}
        if target:
            view['target'] = target
        binary.extend(array.tobytes())
        views.append(view)
        acc = {'bufferView': len(views) - 1, 'componentType': component, 'count': len(array), 'type': kind}
        if minmax:
            acc['min'], acc['max'] = array.min(0).tolist(), array.max(0).tolist()
        accessors.append(acc)
        return len(accessors) - 1

    for material, positions, ids, triangles, *light in primitives:
        pos = np.array(positions, np.float32)
        n = len(pos)
        attrs = {'POSITION': add(pos, 'VEC3', 5126, 34962, True),
                 'NORMAL': add(np.tile(np.float32([0, 1, 0]), (n, 1)), 'VEC3', 5126, 34962),
                 'TEXCOORD_0': add(pos[:, [0, 2]] / 100, 'VEC2', 5126, 34962),
                 '_VERTEX': add(np.array(ids, np.float32), 'SCALAR', 5126, 34962)}
        if light:
            attrs['_FEATURE'] = add(np.array(ids, np.float32), 'SCALAR', 5126, 34962)
            grey = np.rint(np.array(light[0], np.float32) * 255).astype(np.uint8)
            attrs['COLOR_0'] = add(np.stack([grey, grey, grey, np.full(n, 255, np.uint8)], 1), 'VEC4', 5121, 34962)
            accessors[attrs['COLOR_0']]['normalized'] = True
        prims.append({'attributes': attrs, 'indices': add(np.array(triangles, np.uint32).reshape(-1), 'SCALAR', 5125, 34963),
                      'material': material, 'mode': 4})
    doc = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
           'nodes': [{'name': path.stem, 'mesh': 0, 'scale': [.01, .01, .01]}],
           'meshes': [{'name': path.stem, 'primitives': prims}],
           'materials': [{'name': m['name'], 'extras': {'gothic': {'index': i, **m}}} for i, m in enumerate(materials)],
           'accessors': accessors, 'bufferViews': views, 'buffers': [{'byteLength': len(binary)}]}
    text = json.dumps(doc).encode()
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    out = struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(text) + 8 + len(binary))
    out += struct.pack('<II', len(text), 0x4E4F534A) + text + struct.pack('<II', len(binary), 0x004E4942) + bytes(binary)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)


def have_blender():
    try:
        return bool(world_edit.blender_binary())
    except world_edit.EditError:
        return False


blender = pytest.mark.skipif(not have_blender(), reason='no Blender')


FAKE_CONVERTER = '''
import hashlib, json, os, sys
from pathlib import Path
command, world = sys.argv[1], Path(sys.argv[2])
options = dict(zip(sys.argv[3::2], sys.argv[4::2]))
content, out = Path(options['--content']), Path(options['--out'])
if os.environ.get('FAKE_CONVERTER_FAIL'):
    sys.exit('content-convert: ' + os.environ['FAKE_CONVERTER_FAIL'])
if command == 'verify-cooked-world':
    sys.exit(0)
index = hashlib.sha256()
for entry in json.loads((world / 'world.json').read_text())['chunks']:
    file = entry if isinstance(entry, str) else \\
        next(m['file'] for m in entry['meshes'] if m['role'] == 'draw' and m.get('level', 0) == 0)
    glb = content / file if (content / file).is_file() else world / file
    data = b'WCB' + hashlib.sha256(glb.read_bytes()).digest()
    (out / file).with_suffix('.wcb').parent.mkdir(parents=True, exist_ok=True)
    (out / file).with_suffix('.wcb').write_bytes(data)
    index.update(data)
(out / 'cloth.wcl').write_bytes(b'WCL' + index.digest())
(out / 'chunks.wci').write_bytes(b'WCI' + index.digest())
'''


def fake_converter(folder):
    """A stand-in for content-convert cook-world/verify-cooked-world with --content and --out: per cell b'WCB' and
    the SHA-256 of the GLB it reads, the index and cloth bundle b'WCI'/b'WCL' and the SHA-256 of all cells';
    FAKE_CONVERTER_FAIL=<reason> makes it fail."""
    path = folder / 'content-convert'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f'#!{sys.executable}\n' + FAKE_CONVERTER)
    path.chmod(0o755)
    return path


def fake_wcb(glb):
    return b'WCB' + hashlib.sha256(glb.read_bytes()).digest()


def follow_writes(monkeypatch):
    """[(op, path)] of every write and removal world_cook.py makes, in order."""
    import world_cook
    ops = []
    write, remove = world_cook._write, world_cook._remove

    def logged_write(path, data):
        ops.append(('write', path))
        write(path, data)

    def logged_remove(path):
        ops.append(('remove', path))
        remove(path)
    monkeypatch.setattr(world_cook, '_write', logged_write)
    monkeypatch.setattr(world_cook, '_remove', logged_remove)
    return ops
