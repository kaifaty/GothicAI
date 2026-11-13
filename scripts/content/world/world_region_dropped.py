"""The base triangles Blender cannot hold, on the Blender side of build-region (world_region_bpy.py).

The glTF importer drops a triangle with a corner twice and one over the same corners as another (a double-sided
pair); the region's weld removes the triangles left with two corners at one place. The converter has them (up to 10
per chunk of the pilot), and a chunk without them loses border vertices and edges. find() lists them before the
edits; resolve() says after the edits where each goes, and chunk_dropped.py writes them into the chunk:

- a duplicate follows the triangle it repeats (its twin, a face over the same _VERTEX ids: the two sides of a
  double-sided pair have other features, and the weld makes them one face): the twin's
  children after the edits (ORIGIN, a face attribute the children inherit), at their places; chunk_dropped.py turns
  them to the duplicate's winding;
- a degenerate triangle (two corners at one place) lies on the segment between its two places; it goes where the
  vertices of its corners went. When the edits split the segment's edge, it is split alike along the new vertices
  that replaced the edge (a chain of vertices marked new between its two ends), as degenerate triangles again;
- a corner whose vertex the edits removed, a vertex at two places, a twin removed, or a segment whose edge is gone
  with no chain of new vertices in its place fails the build with the triangle's order and place.

A corner whose vertex only dropped triangles have (Blender never had it) follows a corner of the same triangle at
the same place, else stays where it was.
"""
from collections import deque

import bmesh
import numpy as np
from mathutils import Vector

import chunk_canon
import world_bpy

ORIGIN = 'region_face'      # face, INT: the face's index before the edits, plus one; children inherit it
CHAIN = 64                  # vertices a split segment may have between its ends


def remove_degenerate(me):
    """Delete the faces with two corners at one place (before the weld, which would collapse them)."""
    bm = bmesh.new()
    bm.from_mesh(me)
    bad = [f for f in bm.faces if len({tuple(v.co) for v in f.verts}) < len(f.verts)]
    bmesh.ops.delete(bm, geom=bad, context='FACES')
    bm.to_mesh(me)
    bm.free()
    return len(bad)


def _mesh_arrays(me):
    lv = np.empty(len(me.loops), np.int64)
    me.loops.foreach_get('vertex_index', lv)
    vid = np.empty(len(me.vertices), np.float32)
    me.attributes['_VERTEX'].data.foreach_get('value', vid)
    fid = np.empty(len(me.loops), np.float32)
    me.attributes['_FEATURE'].data.foreach_get('value', fid)
    return lv, vid, fid


def _pairs(p, lv, vid, fid):
    return [(int(vid[lv[k]]), int(fid[k])) for k in p.loop_indices]


def _cyclic(a, b):
    return len(a) == len(b) and any(list(a[k:]) + list(a[:k]) == list(b) for k in range(len(a)))


def find(me, chunks, bases, chunk_attribute):
    """The base triangles of each chunk the joined mesh does not have; sets ORIGIN on every face."""
    me.attributes.new(ORIGIN, 'INT', 'FACE').data.foreach_set('value', list(range(1, len(me.polygons) + 1)))
    lv, vid, fid = _mesh_arrays(me)
    chunk = np.empty(len(me.polygons), np.int64)
    me.attributes[chunk_attribute].data.foreach_get('value', chunk)
    faces, by_vids = {}, {}
    for p in me.polygons:
        pairs = _pairs(p, lv, vid, fid)
        face = (p.index, pairs, int(chunk[p.index]))
        faces.setdefault(tuple(sorted(pairs)), []).append(face)
        by_vids.setdefault(tuple(sorted(v for v, _ in pairs)), []).append(face)
    present = {int(v) for v in vid if v >= 0}
    entries = []
    for k, (name, path) in enumerate(zip(chunks, bases), 1):
        rows = {}
        for m, tris, order, *_ in chunk_canon._primitives(path)[1]:
            for t, tri in enumerate(tris if order is not None else ()):
                rows.setdefault(chunk_canon._key(tri), []).append((float(order[t]), tri))
        for key, found in rows.items():
            mine = [f for f in faces.get(key, []) if f[2] == k]
            if len(found) <= len(mine):
                continue
            # its twin: a face over the same vertices (a double-sided pair has other features on its two sides)
            vids = tuple(sorted(v for v, _ in key))
            twins = [f for f in by_vids.get(vids, []) if f[2] == k] or by_vids.get(vids, [])
            twin = (mine or twins or [None])[0]
            # the one Blender dropped is the one whose winding is not the twin's
            found.sort(key=lambda r: twin is not None and _cyclic([int(c['vid']) for c in r[1]],
                                                                   [v for v, _ in twin[1]]))
            for order, tri in found[:len(found) - len(mine)]:
                corners = [[int(c['vid']), int(c['fid']), [float(x) for x in c['pos']]] for c in tri]
                degenerate = len({tuple(c[2]) for c in corners}) < 3
                entries.append({'chunk': name, 'order': order, 'corners': corners,
                                'held': [c[0] in present for c in corners],
                                'kind': 'degenerate' if degenerate else 'duplicate' if twin else 'other',
                                'twin': None if degenerate or twin is None else twin[0] + 1,
                                'twin_pairs': None if degenerate or twin is None else twin[1]})
    return entries


def _chain(bm_verts, start, end, vid):
    """The shortest chain of new vertices (_VERTEX < 0) between two vertices, by the mesh's edges; None if none."""
    seen, queue = {start: None}, deque([start])
    while queue:
        v = queue.popleft()
        depth, back = 0, v
        while seen[back] is not None:
            back, depth = seen[back], depth + 1
        if depth > CHAIN:
            continue
        for e in bm_verts[v].link_edges:
            w = e.other_vert(bm_verts[v]).index
            if w in seen:
                continue
            if w == end:
                path, back = [end], v
                while back is not None:
                    path.append(back)
                    back = seen[back]
                return path[::-1]
            if vid[w] < 0:
                seen[w] = v
                queue.append(w)
    return None


def resolve(me, entries):
    """After the edits (all faces triangles): fill each entry's 'triangles' (corners [_VERTEX, _FEATURE, glTF
    position]), 'unchanged' or 'problem'."""
    lv, vid, fid = _mesh_arrays(me)
    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    origin = np.empty(len(me.polygons), np.int64)
    me.attributes[ORIGIN].data.foreach_get('value', origin)
    by_vid = {}
    for v, i in enumerate(vid):
        if i >= 0:
            by_vid.setdefault(int(i), []).append(v)
    wanted = {e['twin'] for e in entries if e['kind'] == 'duplicate'}
    children = {}
    for p in me.polygons:
        if origin[p.index] in wanted:
            children.setdefault(int(origin[p.index]), []).append(p)

    def gltf(v):
        return [float(x) for x in world_bpy.to_gltf(Vector(co[v]))]
    bm = None
    for e in entries:
        place = f'triangle of order {int(e["order"])} at ({" ".join(f"{x:.2f}" for x in e["corners"][0][2])})'
        if e['kind'] == 'duplicate':
            kids = children.get(e['twin'], [])
            if not kids:
                e['problem'] = f'{place}, which Blender cannot hold (it repeats another): the edits removed the other'
                continue
            e['triangles'] = [[[int(vid[lv[k]]), int(fid[k]), gltf(lv[k])] for k in p.loop_indices] for p in kids]
            e['unchanged'] = len(kids) == 1 and sorted(str([v, pos]) for v, _, pos in e['triangles'][0]) == \
                sorted(str([v, pos]) for v, _, pos in e['corners'])
            continue
        verts, problem = [], None
        for (v, f, pos), held in zip(e['corners'], e['held']):
            places = by_vid.get(v, [])
            if held and not places:
                problem = f'{place}, which Blender cannot hold: the edits removed its vertex {v}'
            elif len(places) > 1:
                problem = f'{place}, which Blender cannot hold: its vertex {v} is at {len(places)} places'
            verts.append(places[0] if held and len(places) == 1 else None)
        if problem:
            e['problem'] = problem
            continue
        for k, (v, f, pos) in enumerate(e['corners']):     # a corner Blender never had follows one at its place
            if verts[k] is None:
                verts[k] = next((verts[j] for j, c in enumerate(e['corners']) if c[2] == pos and verts[j] is not None),
                                None)
        moved = [gltf(verts[k]) if verts[k] is not None else e['corners'][k][2] for k in range(3)]
        tri = [[c[0], c[1], p] for c, p in zip(e['corners'], moved)]
        e['triangles'], e['unchanged'] = [tri], moved == [c[2] for c in e['corners']]
        if e['kind'] != 'degenerate':
            continue
        places = [tuple(c[2]) for c in e['corners']]
        twice = next((p for p in places if places.count(p) > 1), None)
        once = [k for k, p in enumerate(places) if p != twice]
        if not once:
            continue                           # all three corners at one place
        a = next((verts[k] for k, p in enumerate(places) if p == twice and verts[k] is not None), None)
        b = verts[once[0]]
        if a is None or b is None:
            continue
        if bm is None:
            bm = bmesh.new()
            bm.from_mesh(me)
            bm.verts.index_update()
            bm.verts.ensure_lookup_table()
        if any(edge.other_vert(bm.verts[a]).index == b for edge in bm.verts[a].link_edges):
            continue
        chain = _chain(bm.verts, a, b, vid)
        if chain is None:
            e['problem'] = f'{place}, which Blender cannot hold (two corners at one place): the edits removed the edge ' \
                           f'it lies on'
            continue
        pieces, last = [], len(chain) - 2
        for k in range(last + 1):
            piece = []
            for j, c in enumerate(e['corners']):
                own = k == 0 if places[j] == twice else k == last
                at = chain[k] if places[j] == twice else chain[k + 1]
                piece.append([c[0], c[1], moved[j]] if own else [-1, -1, gltf(at)])
            pieces.append(piece)
        e['triangles'], e['unchanged'] = pieces, False
    if bm is not None:
        bm.free()
    return entries
