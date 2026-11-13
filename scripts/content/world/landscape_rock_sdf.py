"""Rock of the landscape pilot rebuilt through a distance field: variant B of the rock probe
(docs/landscape-plan.md, step В2; method note reports/content/2026-11-landscape-pilot/b2-geometry.md, way 4).

An edit of world_edit.py build-region. The same rock as variant A (landscape_rock.py: the same choice, split and
rounding of the ridges), then, instead of moving the vertices of the base's layout:

- the chosen rock becomes a closed body: its surface, a skirt that goes SKIRT_CM under whatever it touches (the
  ground, rock that stays), and a copy of both THICK_CM inside;
- the body becomes a signed distance field of VOXEL_CM (Blender's Mesh to SDF Grid), is smoothed (SDF Grid Mean)
  and comes back as a mesh (Grid to Mesh): sheets that ran into each other are one surface, and the triangles no
  longer follow the base's faces;
- of that mesh the outside is kept (what lies within KEEP_CM of the old surface, without the wall under the skirt), it gets the
  relief of variant A (landscape_rock.depth_at: beds, joints, undercut foot), now on vertices VOXEL_CM apart, and
  is thinned to DECIMATE of its triangles;
- every new triangle takes the material, the chunk and the texture layout of the old face nearest to it (the
  layout continued over the face's plane), and replaces the chosen rock.

The new surface is not sewn to what it touches: it ends under it, on the skirt. What the removed rock leaves open beside
it and the free edge of the new surface are declared open (ctx.allow_open).

Near the waynet and on rock one can stand on the relief keeps variant A's limits; the field itself rounds every
edge by a few centimetres and is not held back there (the walk check says what that does).
"""
import math
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.geometry import barycentric_transform

import landscape_rock as rock
from landscape_ground import smoothstep

VOXEL_CM = 25.0
BAND = 3                 # voxels of the field on each side of the surface
MEAN_WIDTH, MEAN_PASSES = 1, 3
SKIRT_CM = 100.0
THICK_CM = 250.0
KEEP_CM = 110.0
DECIMATE = 0.25          # of the field's triangles stay: about variant A's count
INPUTS = [Path(__file__).with_name('landscape_rock.py'), Path(__file__).with_name('landscape_ground.py')]
PARAMETERS = dict(rock.PARAMETERS, voxel_cm=VOXEL_CM, band=BAND, mean=[MEAN_WIDTH, MEAN_PASSES], skirt_cm=SKIRT_CM,
                  thick_cm=THICK_CM, keep_cm=KEEP_CM, decimate=DECIMATE)


def evaluated(obj):
    return bpy.data.meshes.new_from_object(obj.evaluated_get(bpy.context.evaluated_depsgraph_get()))


def temporary(name, bm):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def discard(obj):
    me = obj.data
    bpy.data.objects.remove(obj)
    bpy.data.meshes.remove(me)


def through_field(obj):
    """The mesh of the object's signed distance field, smoothed."""
    group = bpy.data.node_groups.new('rock_field', 'GeometryNodeTree')
    group.interface.new_socket('Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket('Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    start, end = group.nodes.new('NodeGroupInput'), group.nodes.new('NodeGroupOutput')
    field = group.nodes.new('GeometryNodeMeshToSDFGrid')
    field.inputs['Voxel Size'].default_value = VOXEL_CM
    field.inputs['Band Width'].default_value = BAND
    mean = group.nodes.new('GeometryNodeSDFGridMean')
    mean.inputs['Width'].default_value = MEAN_WIDTH
    mean.inputs['Iterations'].default_value = MEAN_PASSES
    back = group.nodes.new('GeometryNodeGridToMesh')
    group.links.new(start.outputs[0], field.inputs['Mesh'])
    group.links.new(field.outputs[0], mean.inputs['Grid'])
    group.links.new(mean.outputs[0], back.inputs['Grid'])
    group.links.new(back.outputs[0], end.inputs[0])
    modifier = obj.modifiers.new('rock_field', 'NODES')
    modifier.node_group = group
    me = evaluated(obj)
    obj.modifiers.remove(modifier)
    bpy.data.node_groups.remove(group)
    return me


def body(r):
    """The closed body of the chosen rock and its outside as triangles: [(corners, face of the region or None for
    the skirt)]."""
    bm, chosen = r.bm, r.chosen

    def where(k):
        return r.at[k] if k in r.free else Vector(k)
    sb = bmesh.new()
    at_place, source = {}, {}

    def vertex(k):
        if k not in at_place:
            at_place[k] = sb.verts.new(where(k))
        return at_place[k]
    for f in bm.faces:
        if not f[chosen]:
            continue
        places = [r.place_of[v.index] for v in f.verts]
        if len(set(places)) < 3:
            continue
        try:
            source[sb.faces.new([vertex(k) for k in places])] = f
        except ValueError:      # the other side of a double-sided pair
            continue
    sb.normal_update()
    # the skirt: from the outline down under whatever lies there
    under = {}
    for k, v in at_place.items():
        if k in r.free:
            continue
        beside = [f.normal for m in r.members[k] for f in m.link_faces if not f[chosen]]
        out = sum(beside, Vector()).normalized() if beside else Vector()
        way = -out - 0.5 * v.normal
        under[v] = (way.normalized() if way.length > 1e-6 else -v.normal) * SKIRT_CM
    lower = {}
    for e in [e for e in sb.edges if e.is_boundary]:
        loop = e.link_loops[0]
        a, b = loop.vert, loop.link_loop_next.vert
        for v in (a, b):
            if v not in lower:
                lower[v] = sb.verts.new(v.co + under.get(v, -v.normal * SKIRT_CM))
        try:
            sb.faces.new([b, a, lower[a], lower[b]])
        except ValueError:
            pass
    sb.normal_update()
    outside = []
    for f in sb.faces:
        corners = [v.co.copy() for v in f.verts]
        for tri in ([corners] if len(corners) == 3 else [corners[:3], [corners[0], corners[2], corners[3]]]):
            outside.append((tri, source.get(f)))
    # the inside: a copy THICK_CM in, turned, and a wall between the two rims
    inner = {v: sb.verts.new(v.co - v.normal * THICK_CM) for v in list(sb.verts)}
    rim = [(e.link_loops[0].vert, e.link_loops[0].link_loop_next.vert) for e in sb.edges if e.is_boundary]
    for f in list(sb.faces):
        try:
            sb.faces.new([inner[v] for v in reversed(f.verts)])
        except ValueError:
            pass
    for a, b in rim:
        try:
            sb.faces.new([b, a, inner[a], inner[b]])
        except ValueError:
            pass
    return sb, outside


def edit(ctx):
    me = ctx.mesh
    bm = bmesh.new()
    bm.from_mesh(me)
    r = rock.refine(ctx, bm)
    chosen, old = r.chosen, r.old
    uv = bm.loops.layers.uv.verify()
    chunk = ctx.chunk_layer(bm)

    sb, outside = body(r)
    solid = temporary('rock_body', sb)
    sb.free()
    field = through_field(solid)
    discard(solid)

    # the outside of the field's mesh: near the old surface and facing its way
    near = BVHTree.FromPolygons([c for tri, _ in outside for c in tri],
                                [(3 * i, 3 * i + 1, 3 * i + 2) for i in range(len(outside))])
    nb = bmesh.new()
    nb.from_mesh(field)
    bpy.data.meshes.remove(field)
    total = len(nb.faces)
    nb.normal_update()
    drop = []
    for f in nb.faces:
        _, normal, index, distance = near.find_nearest(f.calc_center_median())
        if distance is None or distance > KEEP_CM:
            drop.append(f)
        elif outside[index][1] is None and f.normal.dot(normal) <= 0.3:   # the wall under the skirt's rim
            drop.append(f)
    bmesh.ops.delete(nb, geom=drop, context='FACES')
    bmesh.ops.delete(nb, geom=[v for v in nb.verts if not v.link_faces], context='VERTS')
    bmesh.ops.triangulate(nb, faces=nb.faces[:])
    nb.normal_update()

    # the relief of variant A on the field's vertices
    walk = math.cos(math.radians(rock.WALK_DEG))
    moved = []
    for v in nb.verts:
        foot, _, distance = r.tree.find(v.co)
        limit = rock.way_limit(r, v.co)
        if v.normal.z >= walk:
            limit = min(limit, rock.WALK_LIMIT_CM)
        depth = rock.depth_at(r, v.co, v.normal, foot, distance) * smoothstep(distance / rock.FADE_CM)
        depth = max(-limit, min(limit, depth))
        v.co = v.co + v.normal * depth
        moved.append(abs(depth))
    dense = len(nb.faces)
    thin = temporary('rock_thin', nb)
    nb.free()
    modifier = thin.modifiers.new('thin', 'DECIMATE')
    modifier.ratio = DECIMATE
    thinned = evaluated(thin)
    discard(thin)
    nb = bmesh.new()
    nb.from_mesh(thinned)
    bpy.data.meshes.remove(thinned)
    bmesh.ops.triangulate(nb, faces=[f for f in nb.faces if len(f.verts) > 3])
    nb.normal_update()

    # material, chunk and texture layout from the old face nearest to each new triangle
    real = [(tri, f) for tri, f in outside if f is not None]
    nearest = BVHTree.FromPolygons([c for tri, _ in real for c in tri],
                                   [(3 * i, 3 * i + 1, 3 * i + 2) for i in range(len(real))])
    layout = {}
    for tri, f in real:
        if f not in layout:
            layout[f] = (tri, [Vector((*loop[uv].uv, 0.0)) for loop in f.loops], f.material_index, f[chunk])
    built = []
    for f in nb.faces:
        _, _, index, _ = nearest.find_nearest(f.calc_center_median())
        tri, uvs, material, number = layout[real[index][1]]
        built.append(([v.index for v in f.verts],
                      [barycentric_transform(v.co, *tri, *uvs).to_2d() for v in f.verts], material, number))
    places = [(v.co.copy(), v.normal.copy()) for v in nb.verts]
    nb.free()

    # out with the chosen rock, in with the rebuilt one
    gone = [f for f in bm.faces if f[chosen]]
    held = {v for f in gone for v in f.verts}
    bmesh.ops.delete(bm, geom=gone, context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in held if v.is_valid and not v.link_faces], context='VERTS')
    # what the removed rock leaves open: edges between two places that stay with one face left on them (by place,
    # as the check welds; where three sheets met the outline of the removed faces alone does not say it)
    stay = set(r.stay)
    beside, sides = {}, {}
    for k in stay:
        for v in r.members[k]:
            for f in (v.link_faces if v.is_valid else ()):
                beside[f] = None
    for f in beside:
        for e in f.edges:
            ends = frozenset(rock.key(v.co) for v in e.verts)
            if ends <= stay:
                sides.setdefault(ends, []).append(f)
    for f in {fs[0] for fs in sides.values() if len(fs) == 1}:
        ctx.allow_open([f], 'the rock beside it was rebuilt and ends under it')
    fresh = [bm.verts.new(co) for co, _ in places]
    faces, smooth = [], {}
    for corners, uvs, material, number in built:
        try:
            f = bm.faces.new([fresh[i] for i in corners])
        except ValueError:
            continue
        f.material_index, f[chunk], f.smooth = material, number, True
        for loop, i, at in zip(f.loops, corners, uvs):
            loop[uv].uv = at
            smooth[loop] = places[i][1]
        faces.append(f)
    ctx.mark_new(bm, fresh)
    ctx.allow_open(faces, 'the rebuilt rock ends under the ground and the rock around it')
    normals = []
    for f in bm.faces:
        for loop in f.loops:
            n = smooth.get(loop) or Vector(loop[old])
            normals.append(n.normalized() if n.length > 1e-9 else f.normal.copy())
    bm.loops.layers.float_vector.remove(old)
    bm.faces.layers.int.remove(chosen)
    bm.to_mesh(me)
    bm.free()
    me.normals_split_custom_set(normals)
    print(f'landscape_rock_sdf: {r.before_faces} rock triangles within {rock.RADIUS_CM / 100:.0f} m of '
          f'{rock.WAYPOINT} -> field mesh {total} faces, outside {dense} triangles, thinned to {len(faces)}; '
          f'{len(fresh)} vertices; relief mean {sum(moved) / max(1, len(moved)):.1f} cm, '
          f'max {max(moved, default=0.0):.1f} cm')
