"""A tree's GLB with wind data in COLOR_0: the writer and the reader the checks use. NumPy only, no Blender.

The tree is built Z up in metres (Blender's axes); the file is glTF's: +Y up, metres, counter-clockwise front.
One mesh, one primitive per material, the material's name is the key of `materials.json` (name + `.TGA`).
Per vertex: POSITION, NORMAL (authored, the loader keeps them), TEXCOORD_0 (v grows down the picture) and
COLOR_0 — four normalised bytes, no gamma (reports/engine/2026-11-16-modern-trees/engine-plan.md, task 1):

    R  weight of the branch: 0 at the trunk, 1 at the branch's tip; a card takes the weight of its place
    G  phase of the branch, one for the branch and its cards; 0 on the trunk
    B  weight of the leaf's flutter: 0 at the stem, 1 at the card's free edge; 0 on wood
    A  openness of the crown: 1 outside, 0 in its depth; 1 on the trunk

An optional crown attribute _CARD_ID is an unnormalized unsigned short with the card's rank plus one.
Unmarked primitives, including the wood, omit it.

A card is listed once and its part is `two_sided`: the material's record of materials.json says
`"two_sided": "same"`, the engine draws both sides and lights them by the same normal
(reports/engine/2026-11-16-modern-trees/two-sided.md). In the mode of pairs a part is one-sided and a card is
listed twice, the second time wound the other way over the same vertices.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

import numpy as np

FLOAT, UBYTE, USHORT, UINT = 5126, 5121, 5123, 5125


def to_gltf(vectors: np.ndarray) -> np.ndarray:
    """Z-up (x, y, z) to glTF's Y-up (x, z, -y): a rotation, the winding stays."""
    v = np.asarray(vectors, np.float32)
    return np.stack([v[:, 0], v[:, 2], -v[:, 1]], axis=1)


def from_gltf(vectors: np.ndarray) -> np.ndarray:
    v = np.asarray(vectors, np.float32)
    return np.stack([v[:, 0], -v[:, 2], v[:, 1]], axis=1)


def write(path: Path, parts: list[dict], name: str | None = None, extras: dict | None = None) -> dict:
    """parts: one per material — name, group, cutout, two_sided, position (n, 3) Z up, normal, uv, colour (n, 4)
    uint8, index (t, 3), optional card_id (n,) uint16. Returns the triangle count per material."""
    chunks, views, accessors, primitives, materials, offset = [], [], [], [], [], 0

    def push(array: np.ndarray, target: int | None, kind: str, component: int, *, stride: int = 0, **more) -> int:
        nonlocal offset
        data = array.tobytes()
        view = dict(buffer=0, byteOffset=offset, byteLength=len(data))
        if target is not None:
            view['target'] = target
        if stride:
            view['byteStride'] = stride
        views.append(view)
        data += b'\0' * (-len(data) % 4)
        chunks.append(data)
        offset += len(data)
        accessors.append(dict(bufferView=len(views) - 1, componentType=component, count=len(array), type=kind, **more))
        return len(accessors) - 1

    count = {}
    for part in parts:
        position = to_gltf(part['position'])
        normal = to_gltf(part['normal'])
        normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-9)
        normal = normal.astype(np.float32)
        colour = np.ascontiguousarray(part['colour'], np.uint8)
        index = np.asarray(part['index'])
        if len(position) > 65535:
            raise ValueError(f"{part['name']}: {len(position)} vertices, the loader takes 65535 per material")
        attributes = dict(
            POSITION=push(position, 34962, 'VEC3', FLOAT, min=position.min(axis=0).tolist(),
                          max=position.max(axis=0).tolist()),
            NORMAL=push(normal, 34962, 'VEC3', FLOAT),
            TEXCOORD_0=push(np.ascontiguousarray(part['uv'], np.float32), 34962, 'VEC2', FLOAT),
            COLOR_0=push(colour, 34962, 'VEC4', UBYTE, normalized=True))
        if 'card_id' in part:
            # glTF vertex attributes are aligned to four bytes, including scalar shorts.
            ids = np.zeros((len(position), 2), dtype='<u2')
            ids[:, 0] = part['card_id']
            attributes['_CARD_ID'] = push(ids, 34962, 'SCALAR', USHORT, stride=4)
        primitives.append(dict(attributes=attributes, material=len(materials),
                               indices=push(index.astype(np.uint16).reshape(-1), 34963, 'SCALAR', USHORT)))
        material = dict(name=part['name'], doubleSided=bool(part.get('two_sided')), extras=dict(group=part.get('group', 'WOOD')),
                        pbrMetallicRoughness=dict(metallicFactor=0.0, roughnessFactor=1.0))
        if part.get('cutout'):
            material |= dict(alphaMode='MASK', alphaCutoff=.5)
        materials.append(material)
        count[part['name']] = int(len(index))
    stem = name or path.stem
    mesh = dict(name=stem, primitives=primitives)
    if extras:
        mesh['extras'] = extras
    document = dict(asset=dict(version='2.0', generator='tree_glb.py'), scene=0, scenes=[dict(nodes=[0])],
                    nodes=[dict(mesh=0, name=stem)], meshes=[mesh], materials=materials, accessors=accessors,
                    bufferViews=views, buffers=[dict(byteLength=offset)])
    text = json.dumps(document, separators=(',', ':')).encode()
    text += b' ' * (-len(text) % 4)
    blob = b''.join(chunks)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'glTF' + struct.pack('<II', 2, 28 + len(text) + len(blob))
                     + struct.pack('<I', len(text)) + b'JSON' + text + struct.pack('<I', len(blob)) + b'BIN\0' + blob)
    return count


SHAPES = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}
TYPES = {FLOAT: np.float32, UBYTE: np.uint8, USHORT: np.uint16, UINT: np.uint32}


def read(path: Path) -> list[dict]:
    """The file's primitives back in the builder's axes: name, position, normal, uv, colour (as stored), index."""
    data = Path(path).read_bytes()
    length = struct.unpack_from('<I', data, 12)[0]
    document = json.loads(data[20:20 + length])
    blob = data[28 + length:]

    def accessor(i: int) -> np.ndarray:
        a = document['accessors'][i]
        view = document['bufferViews'][a['bufferView']]
        width, kind = SHAPES[a['type']], np.dtype(TYPES[a['componentType']])
        start = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        stride = view.get('byteStride') or width * kind.itemsize
        raw = np.frombuffer(blob, np.uint8, stride * (a['count'] - 1) + width * kind.itemsize, start)
        if stride != width * kind.itemsize:
            raw = np.lib.stride_tricks.as_strided(raw, (a['count'], width * kind.itemsize), (stride, 1))
        return np.ascontiguousarray(raw).view(kind).reshape(a['count'], width)

    out = []
    for prim in document['meshes'][0]['primitives']:
        at = prim['attributes']
        colour = document['accessors'][at['COLOR_0']] if 'COLOR_0' in at else None
        part = dict(
            name=document['materials'][prim['material']]['name'],
            material=document['materials'][prim['material']],
            position=from_gltf(accessor(at['POSITION'])), normal=from_gltf(accessor(at['NORMAL'])),
            uv=accessor(at['TEXCOORD_0']), index=accessor(prim['indices']).reshape(-1, 3).astype(np.int64),
            colour=accessor(at['COLOR_0']) if colour else None,
            colour_format=(colour['componentType'], colour['type'], bool(colour.get('normalized'))) if colour else None)
        if '_CARD_ID' in at:
            part['card_id'] = accessor(at['_CARD_ID']).reshape(-1)
        out.append(part)
    return out
