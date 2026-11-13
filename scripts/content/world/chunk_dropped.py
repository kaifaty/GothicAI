"""Write the base triangles Blender cannot hold into an edited chunk (world_edit.py build-region).

Plain Python and numpy, like chunk_canon.py. world_region_dropped.py lists them on the Blender side and says where
each went (dropped.json); write() runs after chunk_canon.canonical:

- an unchanged one goes back as the base has it, with its order, flags and lightmap;
- a moved, followed or split one goes into a primitive of new triangles, without an order: its base vertices with
  their base corners (normal, UV, light, _VERTEX, _FEATURE) at their new places, the new vertices of a split or of
  a duplicate's subdivided twin with the corner the export wrote for that place (any chunk of the region);
- a duplicate takes its twin's children in its own winding: when it runs the other way round, each child is turned
  and the normals of its new corners turned with it; its base vertices keep its own corners (a double-sided pair has
  other features and normals on its two sides).

Which base triangle of the same corners is the dropped one does not matter for an unchanged twin: canonical may have
given the twin's export the other one's order, and the one missing in out by order goes back.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np

import chunk_canon


def _pairs(tri):
    return [(int(c['vid']), int(c['fid'])) for c in tri]


def _cyclic(a, b):
    return any(list(a[k:]) + list(a[:k]) == list(b) for k in range(len(a)))


def new_corners(paths) -> dict[bytes, np.void]:
    """The corners of new vertices (_VERTEX < 0) of the edited chunks, by their exact position."""
    out = {}
    for path in paths:
        for _, tris, *_ in chunk_canon._primitives(path)[1]:
            for c in tris.reshape(-1):
                if c['vid'] < 0:
                    out.setdefault(c['pos'].tobytes(), c)
    return out


def write(src: Path, out: Path, entries: list[dict], fresh: dict[bytes, np.void]) -> tuple[dict, list[str]]:
    """Put the dropped triangles of one chunk into out; returns the counts and the problems."""
    base_doc, base = chunk_canon._primitives(src)
    doc, edited = chunk_canon._primitives(out)
    have = {float(o) for _, _, order, _, _ in edited if order is not None for o in order}
    rows, by_key, corners_of = {}, {}, {}
    for m, tris, order, flags, light in base:
        for t, tri in enumerate(tris if order is not None else ()):
            row = (m, tri, order[t], None if flags is None else flags[t], None if light is None else light[t])
            rows[float(order[t])] = row
            by_key.setdefault(chunk_canon._key(tri), []).append(row)
            for c in tri:
                corners_of.setdefault((int(c['vid']), int(c['fid'])), c)
    back, moved, problems, taken = {}, {}, [], set()
    counts = {'unchanged': 0, 'moved': 0, 'triangles': 0}
    for e in entries:
        if e.get('problem'):
            problems.append(e['problem'])
            continue
        row = rows[e['order']]
        if e['order'] in have or e['order'] in taken:
            row = next((r for r in by_key[chunk_canon._key(row[1])] if float(r[2]) not in have | taken), None)
            if row is None:
                continue
        taken.add(float(row[2]))
        m = row[0]
        if e['unchanged']:
            back.setdefault(m['name'], (m, []))[1].append((row[1],) + row[2:])
            counts['unchanged'] += 1
            continue
        tris, turn = e['triangles'], False
        if e['kind'] == 'duplicate' and not _cyclic([v for v, _ in _pairs(row[1])], [p[0] for p in e['twin_pairs']]):
            tris, turn = [t[::-1] for t in tris], True
        own = {int(c['vid']): c for c in row[1]}       # its own corners, also where its twin has other features
        for tri in tris:
            made = []
            for v, f, pos in tri:
                if v >= 0:
                    c = own[v] if v in own else corners_of.get((v, f))
                    if c is None:
                        problems.append(f'triangle of order {int(row[2])}, which Blender cannot hold: its child has '
                                        f'the corner _VERTEX {v} _FEATURE {f}, which the base chunk has not')
                        break
                    c = c.copy()
                else:
                    found = fresh.get(np.array(pos, np.float32).tobytes())
                    if found is None:
                        problems.append(f'triangle of order {int(row[2])}, which Blender cannot hold: no corner of a '
                                        f'new vertex at ({" ".join(f"{x:.2f}" for x in pos)}) to follow')
                        break
                    c = found.copy()
                    if turn:
                        c['nrm'] = -c['nrm']
                c['pos'] = pos
                made.append(c)
            else:
                moved.setdefault(m['name'], (m, []))[1].append(np.array(made, chunk_canon.CORNER))
                counts['triangles'] += 1
        counts['moved'] += 1
    if not back and not moved:
        return counts, problems
    names = [m['name'] for m in doc['materials']]
    for name, (m, _) in list(back.items()) + list(moved.items()):
        if name not in names:          # a material of which out has no triangle
            doc['materials'].append(m)
            names.append(name)
    prims = []
    for m, tris, order, flags, light in edited:
        rows_ = [(tri, None if order is None else order[t], None if flags is None else flags[t],
                  None if light is None else light[t]) for t, tri in enumerate(tris)]
        if order is not None and m['name'] in back:
            rows_ = sorted(rows_ + back.pop(m['name'])[1], key=lambda r: float(r[1]))
        prims.append((names.index(m['name']), rows_, order is not None))
    for name, (m, rows_) in back.items():
        prims.append((names.index(name), sorted(rows_, key=lambda r: float(r[1])), True))
    for name, (m, tris) in moved.items():
        prims.append((names.index(name), [(t, None, None, None) for t in tris], False))
    corners, index = [], {}

    def corner(c):
        b = c.tobytes()
        if b not in index:
            index[b] = len(corners)
            corners.append(c)
        return index[b]
    written = []
    for mi, rows_, source in prims:
        tris = [[corner(c) for c in r[0]] for r in rows_]
        if not source:
            written.append((mi, tris, None, None, None))
            continue
        written.append((mi, tris, np.array([r[1] for r in rows_], np.float32),
                        None if rows_[0][2] is None else np.array([r[2] for r in rows_], np.uint16),
                        None if rows_[0][3] is None else np.array([r[3] for r in rows_], np.float32)))
    chunk_canon._write(out, base_doc, doc, np.array(corners, chunk_canon.CORNER), written)
    return counts, problems
