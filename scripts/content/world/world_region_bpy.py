"""The Blender side of world_edit.py build-region: edit the chunks of a rectangle as one surface.

Started by world_edit.py through world_edit_blender.py, like a chunk build; job.json: {"region": <WORLD>/<x0>,<z0>,
<x1>,<z1>, "chunks": [<x>_<z>, ...], "bases": [<base chunk .glb>, ...], "outs": [<edited .glb>, ...] (one per
chunk, in the same order), "outside": [<_VERTEX id>, ...], "out": <region.json>, "allowed", "meta", "edits",
"names", "world", "repo", "base_chunks", "gui", "held": <held.json>}.

- **Join.** Every chunk is imported, its faces get the integer face attribute CHUNK (the chunk's number: its index
  in ctx.chunks plus one), its materials are shared with the chunks imported before it (the importer makes
  NW_X.001 of a second NW_X: the export would not know the base's name), and all are joined into one object.
  Vertices with the same position, _VERTEX and _FEATURE become one vertex (weld), in one chunk as across two, so
  an edit that moves an inner border vertex or splits an inner border edge moves or splits it for both chunks.
  Copies with another _FEATURE stay separate vertices at the same place, as they are within a chunk: an edit that
  moves a vertex moves every vertex at that place. An edge that the weld closes (an open edge before) is marked
  sharp, so the corners keep the normal fans of their chunk and the base normals come back unchanged.
- **What Blender cannot hold.** The importer drops a base triangle with two corners at one place or over the same
  corners as another; held.json lists, per chunk, the base triangles Blender has after the join, and world_region.py
  writes the others back from the base.
- **Outer border.** A vertex whose _VERTEX a chunk outside the region also has gets the point attribute BORDER
  (1): it may not move or vanish, and an edge between two of them is the region's outer border (do not split it).
- **Underlay:** a copy of the joined original, as for a chunk.
- **Split.** After the edits and the light (world_bpy.bake_light over the whole region) every face goes back to
  the chunk its CHUNK says, the attributes CHUNK and BORDER are dropped and each chunk is exported to its out.
  A face with no chunk (0, or past the list) and a vertex with a _VERTEX or _FEATURE between two ids (made by bmesh
  and not marked new) fail the build with their place.
"""
import json
import resource
import sys
import time
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix

import world_bpy

CHUNK = 'region_chunk'      # face, INT: the index of the face's chunk in ctx.chunks, plus one; 0 is no chunk
BORDER = 'region_border'    # point, INT: 1 on a vertex shared with a chunk outside the region


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
    - as for a chunk, a vertex an edit adds needs _VERTEX and _FEATURE -1 (mark_new), and allow_open declares free
      sides of new geometry. bmesh gives a vertex it makes (a split, a subdivision) the ids of the old ones
      interpolated and copies their tag: count the vertices before (a fresh bmesh puts new ones after its own) and
      mark the rest. A vertex with an id between two fails the build."""

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
        """_VERTEX and _FEATURE -1 on vertices an edit made (bmesh interpolates them between the old ones)."""
        for name in ('_VERTEX', '_FEATURE'):
            layer = bm.verts.layers.float.get(name)
            if layer is not None:
                for v in verts:
                    v[layer] = -1.0


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


def weld(me):
    """Make vertices with the same position, _VERTEX and _FEATURE one vertex; returns how many were merged away."""
    bm = bmesh.new()
    bm.from_mesh(me)
    vid, fid = bm.verts.layers.float.get('_VERTEX'), bm.verts.layers.float.get('_FEATURE')
    if vid is None:
        bm.free()
        return 0
    groups = {}
    for v in bm.verts:
        if v[vid] >= 0:
            groups.setdefault((v.co.x, v.co.y, v.co.z, v[vid], v[fid] if fid else 0.0), []).append(v)
    target = {v: vs[0] for vs in groups.values() for v in vs[1:]}
    for f in bm.faces:                  # never collapse a face: two of its corners onto one vertex
        ends = [target.get(v, v) for v in f.verts]
        if len(set(ends)) < len(ends):
            for v in f.verts:
                target.pop(v, None)
    merged = set(target) | set(target.values())
    for e in bm.edges:
        if e.is_boundary and e.verts[0] in merged and e.verts[1] in merged:
            e.smooth = False            # the fans of the corners stay those of their chunk: the base normals hold
    bmesh.ops.weld_verts(bm, targetmap=target)
    bm.to_mesh(me)
    bm.free()
    return len(target)


def mark_border(me, outside):
    vid = [0.0] * len(me.vertices)
    me.attributes['_VERTEX'].data.foreach_get('value', vid)
    marks = [1 if v >= 0 and int(v) in outside else 0 for v in vid]
    me.attributes.new(BORDER, 'INT', 'POINT').data.foreach_set('value', marks)
    return sum(marks)


def held_triangles(me, chunks):
    """Per chunk, the base triangles Blender holds after the join: each as its sorted (_VERTEX, _FEATURE) pairs.
    The importer drops a triangle with two corners at one place and one over the corners of another; world_region.py
    writes those back from the base."""
    vid, fid = ([0.0] * len(me.vertices) for _ in range(2))
    me.attributes['_VERTEX'].data.foreach_get('value', vid)
    me.attributes['_FEATURE'].data.foreach_get('value', fid)
    values = [0] * len(me.polygons)
    me.attributes[CHUNK].data.foreach_get('value', values)
    held = {name: [] for name in chunks}
    for p, k in zip(me.polygons, values):
        held[chunks[k - 1]].append(sorted([int(vid[v]), int(fid[v])] for v in p.vertices))
    return held


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


def unmarked_vertices(me):
    """Vertices with a _VERTEX or _FEATURE between two ids: bmesh made them (a split, a subdivision) and
    interpolated the ids of the old ones; an edit must mark them new (RegionContext.mark_new)."""
    problems = []
    for name in ('_VERTEX', '_FEATURE'):
        values = [0.0] * len(me.vertices)
        me.attributes[name].data.foreach_get('value', values)
        for v, value in zip(me.vertices, values):
            if value != int(value):
                c = world_bpy.to_gltf(v.co)
                problems.append(f'a vertex at ({c[0]:.2f} {c[1]:.2f} {c[2]:.2f}) has {name} {value:g}: an edit made '
                                f'it without marking it new (ctx.mark_new)')
    return problems


def split(obj, chunks, outs):
    """Export each chunk's faces to its out; returns the problems and the faces per chunk."""
    values, problems = faces_without_chunk(obj.data, len(chunks))
    problems += unmarked_vertices(obj.data)
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
        for attribute in (CHUNK, BORDER):
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
    welded = weld(obj.data)
    border = mark_border(obj.data, set(job['outside']))
    print(f'region {job["region"]}: {len(job["chunks"])} chunks, {len(obj.data.polygons)} faces, {welded} vertices '
          f'welded, {border} on the outer border')
    Path(job['held']).write_text(json.dumps(held_triangles(obj.data, job['chunks'])) + '\n')
    underlay = world_bpy.make_underlay(copy_object(obj))
    ctx = RegionContext(obj, underlay, job.get('world', ''), job['region'], job['chunks'], job.get('base_chunks'))
    meta = world_bpy.run_edits(ctx, job)
    kept, lit = world_bpy.bake_light(obj, underlay)
    print(f'baked light: {kept} base vertices as in the base, {lit} new vertices from the base around them')
    if job.get('gui'):
        bpy.app.handlers.save_pre.append(world_bpy.remove_underlay_on_save)
        return
    world_bpy.remove_underlay()
    Path(job['allowed']).write_text(json.dumps({'edits': ctx.allowed}) + '\n')
    Path(job['meta']).write_text(json.dumps(meta) + '\n')
    problems, faces = split(obj, ctx.chunks, job['outs'])
    result = {'welded': welded, 'border_vertices': border, 'faces': faces, 'problems': problems,
              'light': {'kept': kept, 'new': lit}, 'seconds': round(time.perf_counter() - start, 1),
              'peak_rss_mb': peak_mb()}
    Path(job['out']).write_text(json.dumps(result, indent=1) + '\n')
