"""Write an edited chunk back in the converter's form, its untouched source triangles exactly as in the base chunk.

The Blender glTF exporter does not keep a chunk byte for byte: the normals of the source corners come back with
other float bits (98 % of them, by some 1e-5), the vertex light of a few by one step, and the triangles lose their
order, flags and lightmap (the primitives' extras). The engine's chunk reader (OpenGothic/common/worldchunks.cpp)
keeps a source corner's feature only when its bits match, and puts a triangle without an order after all others,
so every material of the chunk got new meshlets all over the world (PackedMesh::packMeshletsLnd packs one chain
per material): the culled-triangle counter of C1 jumped by +-100-200 thousand per view, and the triangles' flags
and lightmap indices were lost. world_edit.py runs this after the Blender step:

- a source triangle (its three _VERTEX/_FEATURE pairs are one triangle of the base chunk) that the edits left as
  it was, up to the exporter's rounding, is written with the base chunk's corners, corner order, order, flags and
  lightmap;
- any other triangle is new or changed: it keeps the exported values and goes into a primitive of its own, without
  an order, as the reader expects of edited geometry.

Plain Python and numpy, like chunk_glb.py.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

import numpy as np

from chunk_glb import accessor, read_glb

POSITION_CM = 0.01     # cm a source corner may move in the round trip and still be the same corner
UV = 1e-5              # texture units, likewise
NORMAL_DOT = 0.9999    # cosine, likewise
LIGHT = 1              # steps of 255, likewise
FLAT = 1e-5            # a triangle this thin (twice its area over its longest edge squared) has no normal of its own
CORNER = np.dtype([('pos', '<f4', 3), ('nrm', '<f4', 3), ('uv', '<f4', 2), ('col', 'u1', 4), ('vid', '<f4'),
                   ('fid', '<f4')])


def _colour_bytes(doc, binary, index, n):
    if index is None:
        return np.full((n, 4), 255, np.uint8)
    a = accessor(doc, binary, index)
    if a.dtype == np.uint8:
        out = a
    elif a.dtype == np.uint16:
        out = np.rint(a.astype(np.float64) / 257.0).astype(np.uint8)
    else:
        out = np.rint(np.clip(a.astype(np.float64), 0, 1) * 255.0).astype(np.uint8)
    if out.shape[1] == 3:
        out = np.hstack([out, np.full((n, 1), 255, np.uint8)])
    return out


def _primitives(path):
    """[(material record, corners (t, 3) CORNER, order, flags, lightmap or None)] of a chunk."""
    doc, binary = read_glb(path)
    out = []
    for mesh in doc['meshes']:
        for p in mesh['primitives']:
            a = p['attributes']
            n = doc['accessors'][a['POSITION']]['count']
            c = np.zeros(n, CORNER)
            c['pos'] = accessor(doc, binary, a['POSITION'])
            c['nrm'] = accessor(doc, binary, a['NORMAL'])
            c['uv'] = accessor(doc, binary, a['TEXCOORD_0']) if 'TEXCOORD_0' in a else 0
            c['col'] = _colour_bytes(doc, binary, a.get('COLOR_0'), n)
            c['vid'] = accessor(doc, binary, a['_VERTEX']) if '_VERTEX' in a else -1
            c['fid'] = accessor(doc, binary, a['_FEATURE']) if '_FEATURE' in a else -1
            tri = c[accessor(doc, binary, p['indices']).astype(np.int64).reshape(-1, 3)]
            ex = (p.get('extras') or {}).get('gothic') or {}
            extras = tuple(accessor(doc, binary, ex[k]) if k in ex else None for k in ('order', 'flags', 'lightmap'))
            m = doc['materials'][p['material']]
            out.append((m, tri) + extras)
    return doc, out


def _key(tri):
    return tuple(sorted((int(c['vid']), int(c['fid'])) for c in tri))


def _same(a, b):
    """Corners of one source triangle, the edited ones in any rotation: the same up to the round trip. A triangle
    with its corners on a line (the source has them) has no normal of its own; Blender writes another one."""
    p = a['pos'].astype(np.float64)
    longest = max(np.dot(p[i] - p[i - 1], p[i] - p[i - 1]) for i in range(3))
    flat = np.linalg.norm(np.cross(p[1] - p[0], p[2] - p[0])) <= FLAT * longest
    for c in a:
        m = b[(b['vid'] == c['vid']) & (b['fid'] == c['fid'])]
        if len(m) != 1:
            return False
        m = m[0]
        if (np.abs(m['pos'] - c['pos']).max() > POSITION_CM or np.abs(m['uv'] - c['uv']).max() > UV or
                (not flat and float(np.dot(m['nrm'], c['nrm'])) < NORMAL_DOT) or
                np.abs(m['col'].astype(int) - c['col'].astype(int)).max() > LIGHT):
            return False
    return True


def canonical(base_path: Path | str, edited_path: Path | str, out_path: Path | str) -> dict:
    """Write edited_path to out_path in the converter's form (see the module doc); returns counts."""
    base_doc, base = _primitives(base_path)
    source = {}
    for m, tris, order, flags, light in base:
        if order is None:
            continue
        for t, tri in enumerate(tris):
            source[_key(tri)] = (m['name'], tri, order[t], None if flags is None else flags[t],
                                 None if light is None else light[t])
    doc, edited = _primitives(edited_path)
    used = set()
    kept, changed = {}, {}           # material index -> rows
    names = [m['name'] for m in doc['materials']]
    for m, tris, *_ in edited:
        mi = names.index(m['name'])
        for tri in tris:
            k = _key(tri)
            hit = source.get(k) if min(v for v, _ in k) >= 0 and min(f for _, f in k) >= 0 else None
            if hit and k not in used and hit[0] == m['name'] and _same(hit[1], tri):
                used.add(k)
                kept.setdefault(mi, []).append(hit[1:])
            else:
                changed.setdefault(mi, []).append(tri)

    corners, index = [], {}

    def corner(c):
        b = c.tobytes()
        if b not in index:
            index[b] = len(corners)
            corners.append(c)
        return index[b]

    prims = []
    for mi in sorted(set(kept) | set(changed)):
        if mi in kept:
            rows = sorted(kept[mi], key=lambda r: float(r[1]))
            prims.append((mi, [[corner(c) for c in r[0]] for r in rows],
                          np.array([r[1] for r in rows], np.float32),
                          None if rows[0][2] is None else np.array([r[2] for r in rows], np.uint16),
                          None if rows[0][3] is None else np.array([r[3] for r in rows], np.float32)))
        if mi in changed:
            prims.append((mi, [[corner(c) for c in tri] for tri in changed[mi]], None, None, None))
    _write(out_path, base_doc, doc, np.array(corners, CORNER), prims)
    return {'source': len(source), 'kept': sum(map(len, kept.values())),
            'changed': sum(map(len, changed.values()))}


def _write(path, base_doc, doc, corners, prims):
    binary = bytearray()
    views, accessors = [], []

    def add(array, kind, component, target=None, normalized=False, minmax=False):
        array = np.ascontiguousarray(array)
        while len(binary) % 4:
            binary.append(0)
        view = {'buffer': 0, 'byteOffset': len(binary), 'byteLength': array.nbytes}
        if target:
            view['target'] = target
        binary.extend(array.tobytes())
        views.append(view)
        acc = {'bufferView': len(views) - 1, 'componentType': component, 'count': len(array), 'type': kind}
        if normalized:
            acc['normalized'] = True
        if minmax:
            acc['min'], acc['max'] = array.min(0).tolist(), array.max(0).tolist()
        accessors.append(acc)
        return len(accessors) - 1

    attrs = {'POSITION': add(corners['pos'], 'VEC3', 5126, 34962, minmax=True),
             'NORMAL': add(corners['nrm'], 'VEC3', 5126, 34962),
             'TEXCOORD_0': add(corners['uv'], 'VEC2', 5126, 34962),
             'COLOR_0': add(corners['col'], 'VEC4', 5121, 34962, normalized=True),
             '_VERTEX': add(corners['vid'], 'SCALAR', 5126, 34962),
             '_FEATURE': add(corners['fid'], 'SCALAR', 5126, 34962)}
    primitives = []
    for mi, tris, order, flags, light in prims:
        p = {'attributes': attrs, 'indices': add(np.array(tris, np.uint32).reshape(-1), 'SCALAR', 5125, 34963),
             'material': mi, 'mode': 4}
        if order is not None:
            ex = {'order': add(order, 'SCALAR', 5126)}
            if flags is not None:
                ex['flags'] = add(flags, 'VEC2', 5123)
            if light is not None:
                ex['lightmap'] = add(light, 'SCALAR', 5126)
            p['extras'] = {'gothic': ex}
        primitives.append(p)
    node = base_doc['nodes'][0]
    out = {'asset': {**base_doc['asset'], 'generator': 'gothic-ai world_edit'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
           'nodes': [{'name': node.get('name', ''), 'mesh': 0, 'scale': node.get('scale', [.01, .01, .01])}],
           'meshes': [{'name': node.get('name', ''), 'primitives': primitives}],
           'materials': [{'name': m['name'], 'extras': m.get('extras', {})} for m in doc['materials']],
           'accessors': accessors, 'bufferViews': views, 'buffers': [{'byteLength': len(binary)}]}
    text = json.dumps(out).encode()
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    data = struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(text) + 8 + len(binary))
    data += struct.pack('<II', len(text), 0x4E4F534A) + text + struct.pack('<II', len(binary), 0x004E4942) + bytes(binary)
    Path(path).write_bytes(data)


def source_violations(base_path: Path | str, edited_path: Path | str) -> tuple[int, list[str]]:
    """Source triangles of an edited chunk not as the base chunk has them: a triangle with an order whose corners,
    flags or lightmap differ from the base triangle of that order, or a triangle without one that is a source
    triangle the edits left as it was. Returns the number of exact source triangles and the violations."""
    _, base = _primitives(base_path)
    by_order, source = {}, {}
    for m, tris, order, flags, light in base:
        if order is None:
            continue
        for t, tri in enumerate(tris):
            row = (m['name'], tri, None if flags is None else flags[t], None if light is None else light[t])
            by_order[float(order[t])] = row
            source[_key(tri)] = row
    _, edited = _primitives(edited_path)
    exact, bad = 0, []
    for m, tris, order, flags, light in edited:
        for t, tri in enumerate(tris):
            if order is not None:
                want = by_order.get(float(order[t]))
                same = (want is not None and want[0] == m['name'] and want[1].tobytes() == tri.tobytes() and
                        (want[2] is None) == (flags is None) and (flags is None or (want[2] == flags[t]).all()) and
                        (want[3] is None) == (light is None) and (light is None or want[3] == light[t]))
                if same:
                    exact += 1
                else:
                    bad.append(f'triangle of order {int(order[t])} ({m["name"]}) is not the base triangle of that order')
                continue
            k = _key(tri)
            want = source.get(k)
            if want is not None and want[0] == m['name'] and _same(want[1], tri):
                bad.append(f'source triangle {k} ({m["name"]}) lost its order: written back as the exporter left it')
    return exact, bad
