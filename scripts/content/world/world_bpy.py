"""The Blender side of world_edit.py: apply a list of edits to a base chunk of the converted world mesh.

Blender 5.2, started by world_edit.py (world_edit_blender.py is the entry):

    Blender -b --factory-startup --python scripts/content/world/world_edit_blender.py -- <job.json>

job.json: {"base": <base chunk .glb>, "out": <edited .glb>, "allowed": <allowed.json>, "meta": <meta.json>,
"edits": [<edit .py>, ...], "names": {<edit .py>: <path in git>}, "world": <WORLD>, "repo": <repository>,
"chunk": <x>_<z>, "base_chunks": <the base's chunks folder>, "gui": false}. The base chunk is imported twice: the chunk the edits change, and the original as an underlay to
measure against (wireframe, cannot be selected, not rendered). Each edit is a script with one function,
`edit(ctx)`, run in order on the same mesh, so edits of several steps in one chunk add up. The underlay is removed
before the export (and before any save of a .blend in the GUI). After the edits every new vertex takes the baked
light (COLOR_0) of the base vertices around it, and the export keeps COLOR_0 for every material
(keep_vertex_light). allowed.json gets the open edges each edit
declared with ctx.allow_open, in glTF centimetres, for chunk_glb.check; meta.json the PARAMETERS and INPUTS
(data files) of each edit, for the registry.
"""
import importlib.util
import json
import struct
from pathlib import Path

import bpy
from mathutils import Vector
from mathutils.kdtree import KDTree

UNDERLAY = 'underlay'


def to_gothic(v):
    # glTF stores (-x, y, z) cm; Blender's importer turns glTF (x, y, z) into (x, -z, y).
    return Vector((-v.x, v.z, -v.y))


def to_blender(g):
    return Vector((-g.x, -g.z, g.y))


def to_gltf(v):
    return (v.x, v.z, -v.y)


def gothic_name(material):
    record = material.get('gothic') if material else None
    return record.get('name') if record else (material.name if material else '')


class Context:
    """What an edit gets: the chunk's object and mesh, the underlay, the chunk's name, the base's chunks folder
    (to read a neighbour: something that lies across the chunk border), and allow_open."""

    def __init__(self, obj, underlay, world, chunk, base_chunks=None):
        self.obj = obj
        self.mesh = obj.data
        self.underlay = underlay
        self.world = world
        self.chunk = chunk
        self.base_chunks = Path(base_chunks) if base_chunks else None
        self.edit = None
        self.allowed = []     # [{'edit', 'reason', 'edges': [[a, b], ...]}]

    def allow_open(self, faces, reason):
        """Declare that the outline of these bmesh faces may be open in the result: the outline of faces the edit
        removes, or the free sides of what it builds (the sides of a stair, a beam standing on its own). Only the
        outline is declared: an edge two faces of the set share, by exact position as the check welds them, stays
        checked, so a hole inside the new geometry is still caught. Positions are taken now, so call it while the
        faces are valid and placed."""
        count, ends = {}, {}
        for f in faces:
            for e in f.edges:
                a, b = (tuple(struct.unpack('3f', struct.pack('3f', *to_gltf(v.co)))) for v in e.verts)
                key = (a, b) if a <= b else (b, a)
                count[key] = count.get(key, 0) + 1
                ends[key] = [a, b]
        edges = [ends[k] for k, n in count.items() if n == 1]
        self.allowed.append({'edit': self.edit, 'reason': reason, 'edges': edges})


def load_edit(path):
    spec = importlib.util.spec_from_file_location(f'world_edit_{Path(path).stem}', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not callable(getattr(module, 'edit', None)):
        raise SystemExit(f'{path}: an edit needs a function edit(ctx)')
    return module


def import_chunk(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(path))
    return next(o for o in bpy.data.objects if o not in before and o.type == 'MESH')


def make_underlay(obj):
    obj.name = UNDERLAY
    obj.data.name = UNDERLAY
    obj.hide_select = True
    obj.hide_render = True
    obj.display_type = 'WIRE'
    return obj


def remove_underlay():
    obj = bpy.data.objects.get(UNDERLAY)
    if obj is None:
        return
    mesh = obj.data
    materials = [m for m in mesh.materials if m]
    bpy.data.objects.remove(obj)
    bpy.data.meshes.remove(mesh)
    for m in materials:
        if m.users == 0:
            bpy.data.materials.remove(m)


def keep_vertex_light():
    """Make Blender 5.2's glTF exporter write COLOR_0 for every material of the chunk.

    COLOR_0 is the world mesh's baked vertex light (Gothic's feature light), which the engine reads
    (OpenGothic/common/worldchunks.cpp). The exporter's manage_material_info maps the first material using the
    colour attribute to 'COLOR_0' and every later one to the attribute's key ('ColorColor'), which no attribute
    is called, so all but the first primitive came out white, 1.0 (found 2026-10-30, M2: stairs-3 and roofholes-2
    too), and wrote RGB without the light's alpha. The wrapper maps every material to the glTF name of the colour
    it uses and asks for the alpha, before the exporter fills the colour data."""
    from io_scene_gltf2.blender.exp import primitive_extract as pe
    creator = pe.PrimitiveCreator
    fill = '_PrimitiveCreator__manage_color_attributes'     # called at the end of manage_material_info
    original = getattr(creator, fill)
    if getattr(original, 'keeps_vertex_light', False):
        return

    def manage_color_attributes(self):
        gltf = [vc['gltf_name'] for vc in self.vc_infos]
        # the key is the colour's name and, for a material whose node tree does not read it, the alpha's too
        names = {}
        for vc in self.vc_infos:
            if vc.get('forced'):
                continue
            names[(vc['color'] or '') + (vc['alpha'] or '')] = vc['gltf_name']
            names[(vc['color'] or '') * 2] = vc['gltf_name']
        for idx, name in list(self.material_idxs_using_vc.items()):
            if name not in gltf:
                self.material_idxs_using_vc[idx] = names.get(name, gltf[0] if len(gltf) == 1 else name)
        # the light's alpha byte is part of it (ARGB in one word): write RGBA, as the base does
        for vc in self.vc_infos:
            if not vc.get('forced') and vc.get('color'):
                vc['alpha'], vc['add_alpha'] = vc['color'], True
        original(self)
    manage_color_attributes.keeps_vertex_light = True
    setattr(creator, fill, manage_color_attributes)


LIGHT_NEIGHBOURS = 4     # base vertices a new vertex takes its baked light from


def bake_light(obj, underlay):
    """The baked vertex light (COLOR_0, Gothic's feature light) after the edits: a vertex of the base (_VERTEX and
    _FEATURE kept) has exactly its base light on every corner, also on a new face that reuses it; a vertex an edit
    added (_VERTEX -1) takes the inverse-distance mean of the LIGHT_NEIGHBOURS nearest base vertices. An edit's
    own colours are replaced: a new face without light came out white or black, one corner's light carried over a
    whole stair turned it black, a boolean shifted the light of the vertices it kept (M2, 2026-10-30).
    Returns (base vertices relit, new vertices lit)."""
    me, base = obj.data, underlay.data
    colour, base_colour = me.color_attributes.active_color, base.color_attributes.active_color
    ids = [m.attributes.get(n) for m in (me, base) for n in ('_VERTEX', '_FEATURE')]
    if colour is None or base_colour is None or None in ids:
        return 0, 0
    vid, fid, base_vid, base_fid = ids

    def corner(attribute, mesh, loop):
        return tuple(attribute.data[loop.index].color if attribute.domain == 'CORNER'
                     else attribute.data[loop.vertex_index].color)
    known, sums = {}, {}
    for loop in base.loops:
        v = loop.vertex_index
        c = corner(base_colour, base, loop)
        known.setdefault((base_vid.data[v].value, base_fid.data[v].value), c)
        acc = sums.setdefault(v, [0.0, 0.0, 0.0, 0.0, 0])
        for i in range(4):
            acc[i] += c[i]
        acc[4] += 1
    tree = KDTree(len(sums))
    lights = []
    for n, (v, acc) in enumerate(sums.items()):
        tree.insert(base.vertices[v].co, n)
        lights.append([x / acc[4] for x in acc[:4]])
    tree.balance()
    light = {}
    for v in me.vertices:
        key = (vid.data[v.index].value, fid.data[v.index].value)
        if key[0] >= 0 and key in known:
            light[v.index] = known[key]
        elif key[0] < 0:
            found = tree.find_n(v.co, LIGHT_NEIGHBOURS)
            weights = [1.0 / max(d, 1e-3) for _, _, d in found]
            light[v.index] = [sum(w * lights[n][i] for (_, n, _), w in zip(found, weights)) / sum(weights)
                              for i in range(4)]
    if colour.domain == 'CORNER':
        for loop in me.loops:
            if loop.vertex_index in light:
                colour.data[loop.index].color = light[loop.vertex_index]
    else:
        for v, c in light.items():
            colour.data[v].color = c
    new = sum(1 for v in me.vertices if vid.data[v.index].value < 0)
    return len(light) - new, new


def remove_underlay_on_save(*_):
    remove_underlay()


def main(job_path):
    job = json.loads(Path(job_path).read_text())
    bpy.ops.wm.read_factory_settings(use_empty=True)
    obj = import_chunk(job['base'])
    underlay = make_underlay(import_chunk(job['base']))
    world, chunk = job.get('world', ''), job.get('chunk') or Path(job['base']).stem
    ctx = Context(obj, underlay, world, chunk, job.get('base_chunks'))
    meta = {}
    for path in job['edits']:
        ctx.edit = job.get('names', {}).get(path, path)
        print(f'edit {ctx.edit}')
        module = load_edit(path)
        module.edit(ctx)
        # what the registry keeps of the edit besides its script: its constants and the data files it read
        meta[ctx.edit] = {'parameters': getattr(module, 'PARAMETERS', None),
                          'inputs': [Path(p).resolve().relative_to(job['repo']).as_posix()
                                     for p in getattr(module, 'INPUTS', [])]}
    kept, lit = bake_light(obj, underlay)
    print(f'baked light: {kept} base vertices as in the base, {lit} new vertices from the base around them')
    if job.get('gui'):
        bpy.app.handlers.save_pre.append(remove_underlay_on_save)
        return
    remove_underlay()
    Path(job['allowed']).write_text(json.dumps({'edits': ctx.allowed}) + '\n')
    Path(job['meta']).write_text(json.dumps(meta) + '\n')
    tris = sum(len(p.vertices) - 2 for p in obj.data.polygons)
    print(f'chunk: {len(obj.data.polygons)} faces, {tris} triangles after the edits')
    for o in bpy.data.objects:
        o.select_set(o == obj)
    keep_vertex_light()
    bpy.ops.export_scene.gltf(filepath=job['out'], export_format='GLB', use_selection=True, export_extras=True,
                              export_attributes=True, export_vertex_color='ACTIVE', export_all_vertex_colors=True)
