"""Read a chunk of the converted world mesh (.glb) and check an edited chunk against its base chunk.

Plain Python and numpy, no Blender: world_edit.py runs the check after the Blender step, the tests run it on
synthetic chunks. Positions are glTF centimetres (-x, y, z of Gothic) as the chunk stores them; vertices are
compared by their exact float32 bits, as the engine's chunk reader merges them (OpenGothic/common/worldchunks.cpp).

The checks of an edited chunk:
- border: a vertex the base chunk shares with a neighbour chunk (its _VERTEX id is in that chunk too) keeps its
  place. A copy of it at another place, or none left where it was, opens a crack between the chunks;
- open edges: an edge with one triangle (vertices welded by position) is new only if the base chunk did not have
  it open and no edit declared it (world_edit_blender.Context.allow_open);
- source triangles: a triangle the edits left as it was is the base triangle byte for byte, with its order, flags
  and lightmap (chunk_canon.py writes them back after the Blender export);
- materials: each material is in the base world's list (world.json) or its texture is a key of a given
  materials.json;
- report: triangles and area per material, base and edited.
"""
from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

COMPONENTS = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


@dataclass
class Primitive:
    material: dict            # the material's gothic record (extras.gothic), or {'name': <glTF name>}
    positions: np.ndarray     # (t, 3, 3) float32, per triangle corner
    uvs: np.ndarray           # (t, 3, 2) float32
    vertex_ids: np.ndarray    # (t, 3) float32 _VERTEX, -1 for new geometry
    normals: np.ndarray       # (t, 3, 3) float32
    colors: np.ndarray        # (t, 3, 4) float32, 0..1: the baked vertex light (Gothic's feature light)
    feature_ids: np.ndarray = None   # (t, 3) float32 _FEATURE, -1 for new geometry


@dataclass
class Chunk:
    path: Path
    primitives: list[Primitive] = field(default_factory=list)

    @property
    def triangles(self) -> int:
        return sum(len(p.positions) for p in self.primitives)

    def corners(self) -> tuple[np.ndarray, np.ndarray]:
        """All triangle corners: positions (n, 3) and _VERTEX ids (n,)."""
        if not self.primitives:
            return np.zeros((0, 3), np.float32), np.zeros(0, np.float32)
        return (np.concatenate([p.positions.reshape(-1, 3) for p in self.primitives]),
                np.concatenate([p.vertex_ids.reshape(-1) for p in self.primitives]))


def read_glb(path: Path | str) -> tuple[dict, bytes]:
    data = Path(path).read_bytes()
    magic, version, length = struct.unpack_from('<4sII', data, 0)
    if magic != b'glTF' or version != 2:
        raise ValueError(f'{path}: not a glTF 2 binary')
    doc, binary, offset = None, b'', 12
    while offset < length:
        size, kind = struct.unpack_from('<II', data, offset)
        body = data[offset + 8:offset + 8 + size]
        if kind == 0x4E4F534A:
            doc = json.loads(body)
        elif kind == 0x004E4942:
            binary = body
        offset += 8 + size
    if doc is None:
        raise ValueError(f'{path}: no JSON chunk')
    return doc, binary


def accessor(doc: dict, binary: bytes, index: int) -> np.ndarray:
    a = doc['accessors'][index]
    view = doc['bufferViews'][a['bufferView']]
    dtype = np.dtype(COMPONENTS[a['componentType']])
    width = WIDTH[a['type']]
    start = view.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = view.get('byteStride', 0) or dtype.itemsize * width
    count = a['count']
    raw = np.frombuffer(binary, np.uint8, count=stride * (count - 1) + dtype.itemsize * width, offset=start) \
        if count else np.zeros(0, np.uint8)
    if not count:
        return np.zeros((0, width), dtype)
    rows = np.lib.stride_tricks.as_strided(raw, (count, dtype.itemsize * width), (stride, 1))
    out = np.ascontiguousarray(rows).view(dtype).reshape(count, width)
    return out[:, 0] if width == 1 else out


def colour(doc: dict, binary: bytes, index: int) -> np.ndarray:
    a = accessor(doc, binary, index)
    if a.dtype != np.float32:
        a = a.astype(np.float32) / np.float32(np.iinfo(a.dtype).max)
    if a.shape[1] == 3:
        a = np.hstack([a, np.ones((len(a), 1), np.float32)])
    return a


def material_record(doc: dict, index: int | None) -> dict:
    if index is None:
        return {'name': ''}
    m = doc['materials'][index]
    return m.get('extras', {}).get('gothic') or {'name': m.get('name', '')}


def read_chunk(path: Path | str) -> Chunk:
    doc, binary = read_glb(path)
    chunk = Chunk(Path(path))
    for mesh in doc.get('meshes', []):
        for prim in mesh['primitives']:
            if prim.get('mode', 4) != 4:
                raise ValueError(f'{path}: only triangle lists are supported')
            attrs = prim['attributes']
            pos = accessor(doc, binary, attrs['POSITION']).astype(np.float32)
            n = len(pos)
            uv = accessor(doc, binary, attrs['TEXCOORD_0']).astype(np.float32) if 'TEXCOORD_0' in attrs \
                else np.zeros((n, 2), np.float32)
            vid = accessor(doc, binary, attrs['_VERTEX']).astype(np.float32) if '_VERTEX' in attrs \
                else np.full(n, -1, np.float32)
            nor = accessor(doc, binary, attrs['NORMAL']).astype(np.float32) if 'NORMAL' in attrs \
                else np.zeros((n, 3), np.float32)
            col = colour(doc, binary, attrs['COLOR_0']) if 'COLOR_0' in attrs else np.ones((n, 4), np.float32)
            fid = accessor(doc, binary, attrs['_FEATURE']).astype(np.float32) if '_FEATURE' in attrs \
                else np.full(n, -1, np.float32)
            idx = accessor(doc, binary, prim['indices']).astype(np.int64) if 'indices' in prim else np.arange(n)
            tri = idx.reshape(-1, 3)
            chunk.primitives.append(Primitive(material_record(doc, prim.get('material')), pos[tri], uv[tri], vid[tri],
                                              nor[tri], col[tri], fid[tri]))
    return chunk


def position_keys(positions: np.ndarray) -> np.ndarray:
    """Exact positions as rows of three uint32 bit patterns (-0.0 counts as 0.0)."""
    p = np.ascontiguousarray(positions, np.float32).reshape(-1, 3) + np.float32(0)
    return p.view(np.uint32).reshape(-1, 3)


def _key_tuples(keys: np.ndarray) -> list[tuple]:
    return [tuple(int(x) for x in row) for row in keys]


def edge_key(a, b) -> tuple:
    """An undirected edge between two exact positions (each a 3-tuple of bit patterns)."""
    return (a, b) if a <= b else (b, a)


def open_edges(chunk: Chunk) -> set[tuple]:
    """Edges used by exactly one triangle, the triangle corners welded by exact position."""
    positions, _ = chunk.corners()
    if not len(positions):
        return set()
    keys = position_keys(positions).reshape(-1, 3, 3)
    count: dict[tuple, int] = {}
    for tri in keys:
        corners = _key_tuples(tri)
        if len(set(corners)) < 3:
            continue          # degenerate: no edge of its own
        for k in range(3):
            e = edge_key(corners[k], corners[(k + 1) % 3])
            count[e] = count.get(e, 0) + 1
    return {e for e, n in count.items() if n == 1}


def edge_keys_from_floats(edges) -> set[tuple]:
    """Edges given as pairs of float positions (allowed.json) as exact edge keys."""
    out = set()
    for a, b in edges:
        ka, kb = _key_tuples(position_keys(np.array([a, b], np.float32)))
        out.add(edge_key(ka, kb))
    return out


def chunk_coords(name: str) -> tuple[int, int]:
    """'1_-1' -> (1, -1)."""
    head, _, tail = name.rpartition('_')
    return int(head), int(tail)


def shared_vertex_ids(path: Path) -> set[int]:
    """_VERTEX ids of the neighbour chunks of a base chunk (the ids a crack would show at)."""
    x, z = chunk_coords(path.stem)
    ids: set[int] = set()
    for dx in (-1, 0, 1):
        for dz in (-1, 0, 1):
            other = path.with_name(f'{x + dx}_{z + dz}.glb')
            if (dx, dz) == (0, 0) or not other.exists():
                continue
            _, vid = read_chunk(other).corners()
            ids.update(int(v) for v in np.unique(vid) if v >= 0)
    return ids


def border_violations(base: Chunk, edited: Chunk, shared: set[int]) -> list[str]:
    """Border vertices of the base chunk that moved or are gone in the edited one."""
    def places(chunk):
        pos, vid = chunk.corners()
        keys = position_keys(pos)
        out: dict[int, set[tuple]] = {}
        for k, v in zip(_key_tuples(keys), vid):
            v = int(v)
            if v in shared:
                out.setdefault(v, set()).add(k)
        return out, set(_key_tuples(keys))
    before, _ = places(base)
    after, positions = places(edited)
    problems = []
    for v, keys in sorted(before.items()):
        for k in sorted(after.get(v, set()) - keys):
            problems.append(f'border vertex {v} moved to {_floats(k)} (was {", ".join(_floats(q) for q in sorted(keys))})')
        for k in sorted(keys - positions):
            problems.append(f'border vertex {v} at {_floats(k)} is gone: the neighbour chunk still ends there')
    return problems


def _floats(key: tuple) -> str:
    x, y, z = np.array(key, np.uint32).view(np.float32)
    return f'({x:.2f} {y:.2f} {z:.2f})'


def light_violations(base: Chunk, edited: Chunk) -> tuple[int, list[str]]:
    """Corners of the base geometry (_VERTEX and _FEATURE kept) whose vertex light differs from the base's by a
    byte or more: an export that drops COLOR_0 turns them white (Blender 5.2 did for all but the first material).
    Returns how many corners were compared and the violations."""
    def light_bytes(p):
        return np.rint(np.clip(p.colors.reshape(-1, 4), 0, 1) * 255).astype(np.int16)
    known = {}
    for p in base.primitives:
        for v, f, c in zip(p.vertex_ids.reshape(-1), p.feature_ids.reshape(-1), light_bytes(p)):
            known[(int(v), int(f))] = c
    checked, bad = 0, []
    for p in edited.primitives:
        for v, f, c in zip(p.vertex_ids.reshape(-1), p.feature_ids.reshape(-1), light_bytes(p)):
            if v < 0 or f < 0:
                continue
            want = known.get((int(v), int(f)))
            if want is None:
                continue
            checked += 1
            if np.abs(want - c).max() > 1:
                bad.append(f'vertex light of _VERTEX {int(v)} _FEATURE {int(f)} ({p.material.get("name", "")}): '
                           f'{c.tolist()} instead of the base {want.tolist()}')
    return checked, bad


def triangle_area(positions: np.ndarray) -> np.ndarray:
    p = positions.astype(np.float64)
    return np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1) / 2


def material_table(chunk: Chunk) -> dict[str, dict]:
    """Per material name: triangles, area in m² and whether it collides."""
    out: dict[str, dict] = {}
    for p in chunk.primitives:
        name = p.material.get('name', '')
        row = out.setdefault(name, {'triangles': 0, 'area_m2': 0.0,
                                    'collision': not p.material.get('disable_collision', False)})
        row['triangles'] += len(p.positions)
        row['area_m2'] += float(triangle_area(p.positions).sum()) / 1e4
    return out


def known_materials(world_json: Path | None, indexes: list[Path]) -> tuple[set[str], set[str]]:
    """Material names of the base world and texture keys of the given materials.json files."""
    names: set[str] = set()
    if world_json is not None and world_json.exists():
        names = {m['name'] for m in json.loads(world_json.read_text()).get('materials', [])}
    textures: set[str] = set()
    for index in indexes:
        textures.update(k.upper() for k in json.loads(Path(index).read_text()))
    return names, textures


@dataclass
class Report:
    problems: list[str]
    summary: dict

    @property
    def ok(self) -> bool:
        return not self.problems


def check(base_path: Path | str, edited_path: Path | str, *, allowed_open: set[tuple] = frozenset(),
          material_indexes: list[Path] = (), world_json: Path | None = None,
          shared: set[int] | None = None) -> Report:
    base_path, edited_path = Path(base_path), Path(edited_path)
    base, edited = read_chunk(base_path), read_chunk(edited_path)
    if shared is None:
        shared = shared_vertex_ids(base_path)
    if world_json is None:
        world_json = base_path.parent.parent / 'world.json'
    problems: list[str] = []

    border = border_violations(base, edited, shared)
    problems += border

    light_checked, light = light_violations(base, edited)
    problems += light[:20]
    if len(light) > 20:
        problems.append(f'... {len(light) - 20} more corners with another vertex light')

    from chunk_canon import source_violations
    source_exact, source = source_violations(base_path, edited_path)
    problems += source[:20]
    if len(source) > 20:
        problems.append(f'... {len(source) - 20} more source triangles not as the base has them')

    base_open = open_edges(base)
    out_open = open_edges(edited)
    new_open = out_open - base_open - set(allowed_open)
    for a, b in sorted(new_open)[:20]:
        problems.append(f'new open edge {_floats(a)} - {_floats(b)}')
    if len(new_open) > 20:
        problems.append(f'... {len(new_open) - 20} more new open edges')

    names, textures = known_materials(world_json, list(material_indexes))
    table_base, table_out = material_table(base), material_table(edited)
    edited_records = {p.material.get('name', ''): p.material for p in edited.primitives}
    for name, record in sorted(edited_records.items()):
        texture = str(record.get('texture', '')).upper()
        if name in names or (texture and texture in textures):
            continue
        problems.append(f'unknown material {name!r} (texture {record.get("texture")!r}): not in the base world, '
                        f'not in {", ".join(str(p) for p in material_indexes) or "any materials.json"}')

    rows = {}
    for name in sorted(set(table_base) | set(table_out)):
        b = table_base.get(name, {'triangles': 0, 'area_m2': 0.0})
        o = table_out.get(name, {'triangles': 0, 'area_m2': 0.0})
        if b['triangles'] == o['triangles'] and abs(b['area_m2'] - o['area_m2']) < 1e-6:
            continue
        rows[name] = {'triangles': [b['triangles'], o['triangles']],
                      'area_m2': [round(b['area_m2'], 4), round(o['area_m2'], 4)],
                      'collision': (table_out.get(name) or table_base[name])['collision']}
    shared_in_base = {int(v) for v in base.corners()[1] if int(v) in shared}
    summary = {
        'triangles': [base.triangles, edited.triangles],
        'border_vertices': len(shared_in_base),
        'open_edges': {'base': len(base_open), 'edited': len(out_open), 'declared': len(allowed_open),
                       'new': len(new_open)},
        'materials': rows,
        'light': {'checked': light_checked, 'other': len(light)},
        'source': {'exact': source_exact, 'other': len(source)},
    }
    return Report(problems, summary)


def format_report(report: Report) -> str:
    s = report.summary
    lines = [f'triangles {s["triangles"][0]} -> {s["triangles"][1]} ({s["triangles"][1] - s["triangles"][0]:+d}); '
             f'border vertices {s["border_vertices"]}; open edges base {s["open_edges"]["base"]}, edited '
             f'{s["open_edges"]["edited"]}, declared by the edits {s["open_edges"]["declared"]}, new {s["open_edges"]["new"]}; '
             f'vertex light of {s["light"]["checked"]} base corners, {s["light"]["other"]} changed; '
             f'source triangles exact {s.get("source", {}).get("exact", 0)}, other {s.get("source", {}).get("other", 0)}']
    for name, row in s['materials'].items():
        (t0, t1), (a0, a1) = row['triangles'], row['area_m2']
        lines.append(f'  {name:32} triangles {t0:6d} -> {t1:6d} ({t1 - t0:+6d})   area {a0:9.3f} -> {a1:9.3f} m² '
                     f'({a1 - a0:+8.3f}){"" if row["collision"] else "  no collision"}')
    lines += [f'FAIL: {p}' for p in report.problems]
    return '\n'.join(lines)


def _bits(values: np.ndarray, width: int) -> np.ndarray:
    return (np.ascontiguousarray(values, np.float32) + np.float32(0)).view(np.uint32).reshape(-1, 3, width)


def canonical_triangles(chunk: Chunk, full: bool = False) -> dict[str, list[tuple]]:
    """Per material: sorted triangles, each as its three (position bits, uv bits) corners rotated to start at the
    smallest corner (the winding stays). Two chunks with equal tables have the same triangles, UV and materials.
    With full, a corner also holds its normal, colour and _VERTEX, and the material its whole record."""
    out: dict[str, list[tuple]] = {}
    for p in chunk.primitives:
        parts = [position_keys(p.positions).reshape(-1, 3, 3), _bits(p.uvs, 2)]
        if full:
            parts += [_bits(p.normals, 3), _bits(p.colors, 4), _bits(p.vertex_ids, 1)]
        name = p.material.get('name', '')
        rows = out.setdefault(json.dumps(p.material, sort_keys=True) if full else name, [])
        for tri in zip(*parts):
            corners = [sum((tuple(int(x) for x in part[k]) for part in tri), ()) for k in range(3)]
            k = corners.index(min(corners))
            rows.append(tuple(corners[k:] + corners[:k]))
    return {name: sorted(rows) for name, rows in out.items()}


def same_geometry(a: Path | str, b: Path | str, full: bool = False) -> list[str]:
    """Differences between two chunks in triangles, UV and materials (with full: also normals, colours, _VERTEX
    and material records); empty when they are equal."""
    ta, tb = canonical_triangles(read_chunk(a), full), canonical_triangles(read_chunk(b), full)
    out = []
    for name in sorted(set(ta) | set(tb)):
        x, y = ta.get(name, []), tb.get(name, [])
        if x != y:
            only_a = len(set(x) - set(y))
            only_b = len(set(y) - set(x))
            out.append(f'{name}: {len(x)} vs {len(y)} triangles, {only_a} only in the first, {only_b} only in the second')
    return out
