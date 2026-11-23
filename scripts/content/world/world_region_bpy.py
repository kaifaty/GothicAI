"""The Blender side of world_edit.py build-region: edit the chunks of a rectangle as one surface.

Started by world_edit.py through world_edit_blender.py, like a chunk build; job.json: {"region": <WORLD>/<x0>,<z0>,
<x1>,<z1>, "chunks": [<x>_<z>, ...], "bases": [<base chunk .glb>, ...], "outs": [<edited .glb>, ...] (one per
chunk, in the same order), "outside": [<_VERTEX id>, ...], "out": <region.json>, "allowed", "meta", "edits",
"names", "world", "repo", "base_chunks", "gui", "dropped": <dropped.json>}.

- **Join.** Every chunk is imported, its faces get the integer face attribute CHUNK (the chunk's number: its index
  in ctx.chunks plus one), its materials are shared with the chunks imported before it (the importer makes
  NW_X.001 of a second NW_X: the export would not know the base's name), and all are joined into one object.
  _FEATURE becomes a face-corner attribute (each corner keeps the feature it had), and the vertices with the same
  position and _VERTEX become one vertex (weld), in one chunk as across two: the joined mesh is one surface for
  bmesh, an inner border or a feature seam is an ordinary edge. Every corner gets its base normal again after the
  weld (Blender keeps a custom normal relative to the corner's fan, and the weld changes fans), and an edge the
  weld closes (two open edges before) is marked sharp, so the fans stay as they were where they can. The
  export writes the corner _FEATURE per vertex again, splitting a vertex where its corners differ, as the base has it.
- **What Blender cannot hold** (world_region_dropped.py): the triangles the importer drops and those the weld
  would collapse are listed before the edits and resolved after them; dropped.json takes them to world_region.py.
- **Outer border.** A vertex whose _VERTEX a chunk outside the region also has gets the point attribute BORDER
  (1): it may not move or vanish, and an edge between two of them is the region's outer border (do not split it).
- **Underlay:** a copy of the joined original, as for a chunk.
- **Split.** After the edits, n-gons are triangulated, the light is baked per corner over the whole region
  (bake_light: a base corner keeps its light, a new vertex takes the base around it, as world_bpy.bake_light does)
  and every face goes back to the chunk its CHUNK says; the attributes CHUNK, BORDER and ORIGIN are dropped, a
  corner of a new vertex gets _FEATURE -1, and each chunk is exported to its out. A face with no chunk (0, or past
  the list), a _VERTEX or _FEATURE between two ids (made by bmesh and not marked new) and a corner of a base vertex
  with a feature the base does not give that vertex fail the build with their place.
"""
import json
import resource
import sys
import time
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix
from mathutils.kdtree import KDTree

import world_bpy
import world_region_dropped as dropped

CHUNK = 'region_chunk'      # face, INT: the index of the face's chunk in ctx.chunks, plus one; 0 is no chunk
BORDER = 'region_border'    # point, INT: 1 on a vertex shared with a chunk outside the region
NORMAL = 'region_normal'    # corner, FLOAT_VECTOR: the base normal through the weld


class RegionContext(world_bpy.Context):
    """What an edit of a region gets: as world_bpy.Context, with obj and mesh the joined chunks of the region,
    chunk None and besides:

    - region: '<WORLD>/<x0>,<z0>,<x1>,<z1>'; chunks: the region's chunk names ('<x>_<z>') in their order;
    - the face attribute CHUNK ('region_chunk'): the face's chunk, its index in chunks plus one. Faces an edit makes
      from a face (bmesh subdivide_edges, triangulate, a knife cut, bm.faces.new(verts, example)) carry the
      parent's value over, and keep its chunk. A face an edit makes from nothing (bm.faces.new(verts)) has 0: the
      edit must set it (face[ctx.chunk_layer(bm)] = ctx.chunk_number('5_-1')), else the build fails naming the
      face's place. Triangles are not re-sorted by their centre: children stay in their parent's chunk;
    - the point attribute BORDER ('region_border'): 1 on the region's outer border. Such a vertex may not move or
      vanish, and an edge between two of them may not be split (a new open edge of the region);
    - one vertex per _VERTEX at a place, in a chunk and across the chunks: move a vertex and every chunk and every
      side of a feature seam follows; split an edge and both faces on it are split. _FEATURE is a face-corner
      attribute here (bm.loops.layers.float['_FEATURE']), not a vertex one; the underlay has it the same way;
    - as for a chunk, a vertex an edit adds needs _VERTEX -1 (mark_new; the corners of a new vertex get _FEATURE -1
      at the split), and allow_open declares free sides of new geometry. bmesh gives a vertex it makes (a split, a
      subdivision) the ids of the old ones interpolated and copies their tag: count the vertices before (a fresh
      bmesh puts new ones after its own) and mark the rest. A vertex with an id between two fails the build. A new
      face over old vertices takes their corners' features from an example face (bm.faces.new(verts, example)) or
      from the edit; a base vertex's corner with a feature the base does not give it fails the build."""

    def __init__(self, obj, underlay, world, region, chunks, base_chunks=None):
        super().__init__(obj, underlay, world, None, base_chunks)
        self.region = region
        self.chunks = list(chunks)

    def chunk_number(self, name):
        """The CHUNK value of a chunk of the region, '<x>_<z>'."""
        return self.chunks.index(name) + 1

    @staticmethod
    def chunk_layer(bm):
        return bm.faces.layers.int[CHUNK]

    @staticmethod
    def border_layer(bm):
        return bm.verts.layers.int[BORDER]

    @staticmethod
    def mark_new(bm, verts):
        """_VERTEX -1 on vertices an edit made, and _FEATURE -1 on their corners (bmesh interpolates both)."""
        vid, fid = bm.verts.layers.float['_VERTEX'], bm.loops.layers.float['_FEATURE']
        for v in verts:
            v[vid] = -1.0
            for loop in v.link_loops:
                loop[fid] = -1.0


def share_materials(objects):
    """One material per Gothic name across the imported chunks: the first import's, which has the base's name."""
    first = {}
    for o in objects:
        for i, m in enumerate(o.data.materials):
            keep = first.setdefault(world_bpy.gothic_name(m), m)
            if keep is not m:
                o.data.materials[i] = keep
    for m in list(bpy.data.materials):
        if m.users == 0:
            bpy.data.materials.remove(m)


def import_region(paths):
    """The chunks joined into one object, CHUNK on every face."""
    objects = []
    for k, path in enumerate(paths, 1):
        o = world_bpy.import_chunk(path)
        o.data.attributes.new(CHUNK, 'INT', 'FACE').data.foreach_set('value', [k] * len(o.data.polygons))
        objects.append(o)
    share_materials(objects)
    matrix = objects[0].matrix_world.copy()
    for o in objects:
        o.matrix_world = Matrix.Identity(4)   # the join then copies the positions without arithmetic
    for o in bpy.data.objects:
        o.select_set(o in objects)
    bpy.context.view_layer.objects.active = objects[0]
    if len(objects) > 1:
        bpy.ops.object.join()
    obj = objects[0]
    obj.matrix_world = matrix
    return obj


def feature_to_corners(me):
    """_FEATURE from a vertex attribute to a face-corner one, each corner with its vertex's feature."""
    vertex = np.empty(len(me.vertices), np.float32)
    me.attributes['_FEATURE'].data.foreach_get('value', vertex)
    loops = np.empty(len(me.loops), np.int64)
    me.loops.foreach_get('vertex_index', loops)
    me.attributes.remove(me.attributes['_FEATURE'])
    me.attributes.new('_FEATURE', 'FLOAT', 'CORNER').data.foreach_set('value', vertex[loops])


def weld(me):
    """Make the vertices with the same position and _VERTEX one vertex. Returns how many were merged away and how
    many of those had another feature than the vertex they joined (a feature seam closed)."""
    normals = np.empty(len(me.loops) * 3, np.float32)
    me.corner_normals.foreach_get('vector', normals)
    me.attributes.new(NORMAL, 'FLOAT_VECTOR', 'CORNER').data.foreach_set('vector', normals)
    bm = bmesh.new()
    bm.from_mesh(me)
    bm.verts.index_update()
    bm.verts.ensure_lookup_table()
    vid, fid = bm.verts.layers.float['_VERTEX'], bm.loops.layers.float['_FEATURE']
    groups = {}
    for v in bm.verts:
        if v[vid] >= 0:
            groups.setdefault((v.co.x, v.co.y, v.co.z, v[vid]), []).append(v.index)
    target = {v: vs[0] for vs in groups.values() for v in vs[1:]}
    for f in bm.faces:                  # never collapse a face: two of its corners onto one vertex
        ends = [target.get(v.index, v.index) for v in f.verts]
        if len(set(ends)) < len(ends):
            for v in f.verts:
                target.pop(v.index, None)
    pairs = {}
    for e in bm.edges:
        pairs.setdefault(frozenset(target.get(v.index, v.index) for v in e.verts), []).append(e)
    for edges in pairs.values():
        if len(edges) > 1:
            for e in edges:
                e.smooth = False        # the corners keep their fans: the base normals hold

    def feature(i):
        loops = bm.verts[i].link_loops
        return loops[0][fid] if loops else None
    seams = sum(1 for v, t in target.items() if feature(v) != feature(t))
    bmesh.ops.weld_verts(bm, targetmap={bm.verts[v]: bm.verts[t] for v, t in target.items()})
    bm.to_mesh(me)
    bm.free()
    # Blender keeps a custom normal relative to its corner's fan; the weld changes fans (and makes one face of a
    # double-sided pair), so the corners get their normals again, kept per corner through the weld
    normals = np.empty(len(me.loops) * 3, np.float32)
    me.attributes[NORMAL].data.foreach_get('vector', normals)
    me.normals_split_custom_set(normals.reshape(-1, 3))
    me.attributes.remove(me.attributes[NORMAL])
    return len(target), seams


def mark_border(me, outside):
    vid = [0.0] * len(me.vertices)
    me.attributes['_VERTEX'].data.foreach_get('value', vid)
    marks = [1 if v >= 0 and int(v) in outside else 0 for v in vid]
    me.attributes.new(BORDER, 'INT', 'POINT').data.foreach_set('value', marks)
    return sum(marks)


def copy_object(obj):
    copy = obj.copy()
    copy.data = obj.data.copy()
    bpy.context.scene.collection.objects.link(copy)
    return copy


def faces_without_chunk(me, count):
    values = [0] * len(me.polygons)
    me.attributes[CHUNK].data.foreach_get('value', values)
    problems = []
    for p, k in zip(me.polygons, values):
        if not 1 <= k <= count:
            c = world_bpy.to_gltf(p.center)
            problems.append(f'a face at ({c[0]:.2f} {c[1]:.2f} {c[2]:.2f}) has no chunk ({CHUNK} {k}): an edit made '
                            f'it from nothing without setting {CHUNK}')
    return values, problems


def unmarked_vertices(me, pairs):
    """Corners with a _VERTEX or _FEATURE between two ids (bmesh made the vertex and interpolated the old ids: an
    edit must mark it new, RegionContext.mark_new), or a base vertex with a feature the base does not give it."""
    lv, vid, fid = dropped._mesh_arrays(me)
    problems = []
    for k, v in enumerate(lv):
        i, f = float(vid[v]), float(fid[k])
        if i != int(i) or f != int(f):
            what = f'_VERTEX {i:g}' if i != int(i) else f'_FEATURE {f:g}'
            why = 'an edit made it without marking it new (ctx.mark_new)'
        elif i >= 0 and (int(i), int(f)) not in pairs:
            what, why = f'_VERTEX {int(i)} _FEATURE {int(f)}', 'the base does not give that vertex this feature'
        else:
            continue
        c = world_bpy.to_gltf(me.vertices[v].co)
        problems.append(f'a corner at ({c[0]:.2f} {c[1]:.2f} {c[2]:.2f}) has {what}: {why}')
        if len(problems) > 20:
            break
    return problems


def bake_light(obj, underlay):
    """world_bpy.bake_light with the feature per corner: a corner of a base vertex with a base feature has exactly its
    base light, a vertex an edit added takes the inverse-distance mean of the LIGHT_NEIGHBOURS nearest base vertices,
    a vertex an edit lit itself (world_bpy.keep_own_light) keeps its light. Returns (base corners, new vertices)."""
    me, base = obj.data, underlay.data
    colour, base_colour = me.color_attributes.active_color, base.color_attributes.active_color
    if colour is None or base_colour is None:
        return 0, 0
    if colour.domain != 'CORNER' or base_colour.domain != 'CORNER':
        raise SystemExit('build-region: the vertex light must be a face-corner attribute, as the importer makes it')

    def light(m, attribute):
        out = np.empty(len(m.loops) * 4, np.float32)
        attribute.data.foreach_get('color', out)
        return out.reshape(-1, 4)
    blv, bvid, bfid = dropped._mesh_arrays(base)
    bcol = light(base, base_colour)
    known = {}
    for k in range(len(blv)):
        known.setdefault((int(bvid[blv[k]]), int(bfid[k])), bcol[k])
    sums = np.zeros((len(base.vertices), 4))
    counts = np.zeros(len(base.vertices))
    np.add.at(sums, blv, bcol)
    np.add.at(counts, blv, 1)
    lit = np.flatnonzero(counts)
    tree = KDTree(len(lit))
    for n, v in enumerate(lit):
        tree.insert(base.vertices[int(v)].co, n)
    tree.balance()
    means = sums[lit] / counts[lit, None]
    lv, vid, fid = dropped._mesh_arrays(me)
    col = light(me, colour)
    own = me.attributes.get(world_bpy.OWN_LIGHT)
    own_marks = None
    if own is not None:
        own_marks = np.empty(len(me.vertices), np.float32)
        own.data.foreach_get('value', own_marks)
    new, kept = {}, 0
    for k, v in enumerate(lv):
        if own_marks is not None and own_marks[v] > 0:
            continue
        key = (int(vid[v]), int(fid[k]))
        if key[0] >= 0 and key in known:
            col[k] = known[key]
            kept += 1
        elif key[0] < 0:
            if v not in new:
                found = tree.find_n(me.vertices[int(v)].co, world_bpy.LIGHT_NEIGHBOURS)
                weights = np.array([1.0 / max(d, 1e-3) for _, _, d in found])
                new[v] = (weights[:, None] * means[[n for _, n, _ in found]]).sum(0) / weights.sum()
            col[k] = new[v]
    colour.data.foreach_set('color', col.reshape(-1))
    if own is not None:
        me.attributes.remove(own)
    return kept, len(new)


def finish(obj):
    """Before the split: n-gons to triangles (the export would cut them its own way), _FEATURE -1 on the corners of
    new vertices."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    vid, fid = bm.verts.layers.float['_VERTEX'], bm.loops.layers.float['_FEATURE']
    for v in bm.verts:
        if v[vid] < 0:
            for loop in v.link_loops:
                loop[fid] = -1.0
    bm.to_mesh(obj.data)
    bm.free()


def split(obj, chunks, outs, pairs):
    """Export each chunk's faces to its out; returns the problems and the faces per chunk."""
    values, problems = faces_without_chunk(obj.data, len(chunks))
    problems += unmarked_vertices(obj.data, pairs)
    if problems:
        return problems[:20] + ([f'... {len(problems) - 20} more'] if len(problems) > 20 else []), {}
    world_bpy.keep_vertex_light()
    faces = {}
    for k, (name, out) in enumerate(zip(chunks, outs), 1):
        faces[name] = values.count(k)
        if not faces[name]:
            problems.append(f'chunk {name} has no faces left')
            continue
        part = copy_object(obj)
        bm = bmesh.new()
        bm.from_mesh(part.data)
        layer = bm.faces.layers.int[CHUNK]
        bmesh.ops.delete(bm, geom=[f for f in bm.faces if f[layer] != k], context='FACES')
        bm.to_mesh(part.data)
        bm.free()
        for attribute in (CHUNK, BORDER, dropped.ORIGIN):
            if part.data.attributes.get(attribute) is not None:
                part.data.attributes.remove(part.data.attributes[attribute])
        for o in bpy.data.objects:
            o.select_set(o == part)
        bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', use_selection=True, export_extras=True,
                                  export_attributes=True, export_vertex_color='ACTIVE', export_all_vertex_colors=True)
        mesh = part.data
        bpy.data.objects.remove(part)
        bpy.data.meshes.remove(mesh)
    return problems, faces


def peak_mb():
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return round(rss / (1024 * 1024 if sys.platform == 'darwin' else 1024), 1)


def main(job_path):
    start = time.perf_counter()
    job = json.loads(Path(job_path).read_text())
    bpy.ops.wm.read_factory_settings(use_empty=True)
    obj = import_region(job['bases'])
    degenerate = dropped.remove_degenerate(obj.data)
    feature_to_corners(obj.data)
    welded, seams = weld(obj.data)
    border = mark_border(obj.data, set(job['outside']))
    print(f'region {job["region"]}: {len(job["chunks"])} chunks, {len(obj.data.polygons)} faces, {welded} vertices '
          f'welded ({seams} feature seams closed), {border} on the outer border, {degenerate} faces with two corners '
          f'at one place set aside')
    entries = dropped.find(obj.data, job['chunks'], job['bases'], CHUNK)
    lv, vid, fid = dropped._mesh_arrays(obj.data)
    pairs = {(int(vid[v]), int(fid[k])) for k, v in enumerate(lv)} | \
        {(c[0], c[1]) for e in entries for c in e['corners']}
    underlay = world_bpy.make_underlay(copy_object(obj))
    ctx = RegionContext(obj, underlay, job.get('world', ''), job['region'], job['chunks'], job.get('base_chunks'))
    meta = world_bpy.run_edits(ctx, job)
    finish(obj)
    kept, lit = bake_light(obj, underlay)
    print(f'baked light: {kept} base corners as in the base, {lit} new vertices from the base around them')
    if job.get('gui'):
        bpy.app.handlers.save_pre.append(world_bpy.remove_underlay_on_save)
        return
    world_bpy.remove_underlay()
    Path(job['allowed']).write_text(json.dumps({'edits': ctx.allowed}) + '\n')
    Path(job['meta']).write_text(json.dumps(meta) + '\n')
    Path(job['dropped']).write_text(json.dumps(dropped.resolve(obj.data, entries)) + '\n')
    problems, faces = split(obj, ctx.chunks, job['outs'], pairs)
    result = {'welded': welded, 'feature_seams': seams, 'border_vertices': border, 'faces': faces,
              'problems': problems, 'light': {'kept': kept, 'new': lit}, 'dropped': len(entries),
              'seconds': round(time.perf_counter() - start, 1), 'peak_rss_mb': peak_mb()}
    Path(job['out']).write_text(json.dumps(result, indent=1) + '\n')
